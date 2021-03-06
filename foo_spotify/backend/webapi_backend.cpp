#include <stdafx.h>

#include "webapi_backend.h"

#include <backend/webapi_auth.h>
#include <backend/webapi_objects/webapi_media_objects.h>
#include <backend/webapi_objects/webapi_paging_object.h>
#include <backend/webapi_objects/webapi_user.h>
#include <fb2k/advanced_config.h>
#include <utils/abort_manager.h>
#include <utils/json_std_extenders.h>
#include <utils/sleeper.h>

#include <component_urls.h>

#include <qwr/fb2k_adv_config.h>
#include <qwr/file_helpers.h>
#include <qwr/final_action.h>
#include <qwr/string_helpers.h>
#include <qwr/type_traits.h>
#include <qwr/winapi_error_helpers.h>

#include <filesystem>
#include <unordered_set>

// TODO: replace unique_ptr with shared_ptr wherever needed to avoid copying

namespace fs = std::filesystem;

namespace
{

constexpr size_t kRpsLimit = 2;

}

namespace sptf
{

WebApi_Backend::WebApi_Backend( AbortManager& abortManager )
    : abortManager_( abortManager )
    , shouldLogWebApiRequest_( config::advanced::logging_webapi_request )
    , shouldLogWebApiResponse_( config::advanced::logging_webapi_response )
    , rpsLimiter_( kRpsLimit )
    , client_( url::spotifyApi, GetClientConfig() )
    , trackCache_( "tracks" )
    , artistCache_( "artists" )
    , albumImageCache_( "albums" )
    , artistImageCache_( "artists" )
    , pAuth_( std::make_unique<WebApiAuthorizer>( GetClientConfig(), abortManager ) )
{
}

WebApi_Backend::~WebApi_Backend()
{
}

void WebApi_Backend::Finalize()
{
    cts_.cancel();
    pAuth_.reset();
}

WebApiAuthorizer& WebApi_Backend::GetAuthorizer()
{
    return *pAuth_;
}

std::unique_ptr<const sptf::WebApi_User> WebApi_Backend::GetUser( abort_callback& abort )
{
    if ( auto userOpt = userCache_.GetObjectFromCache();
         userOpt )
    {
        return std::unique_ptr<sptf::WebApi_User>( std::move( *userOpt ) );
    }
    else
    {
        web::uri_builder builder;
        builder.append_path( L"me" );

        const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
        auto ret = responseJson.get<std::unique_ptr<WebApi_User>>();

        userCache_.CacheObject( *ret );
        return std::unique_ptr<sptf::WebApi_User>( std::move( ret ) );
    }
}

void WebApi_Backend::RefreshCacheForTracks( nonstd::span<const std::string> trackIds, abort_callback& abort )
{
    constexpr size_t kMaxItemsPerRequest = 50;

    // remove duplicates
    const auto uniqueIds = trackIds | ranges::to<std::unordered_set<std::string>>;

    for ( const auto& trackIdsChunk:
          uniqueIds
              | ranges::views::remove_if( [&]( const auto& id ) { return trackCache_.IsCached( id ); } )
              | ranges::views::chunk( kMaxItemsPerRequest ) )
    {
        const auto trackIdsStr = qwr::unicode::ToWide( qwr::string::Join( trackIdsChunk | ranges::to_vector, ',' ) );

        web::uri_builder builder;
        builder
            .append_path( L"tracks" )
            .append_query( L"ids", trackIdsStr );

        const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
        const auto tracksIt = responseJson.find( "tracks" );
        qwr::QwrException::ExpectTrue( responseJson.cend() != tracksIt,
                                       L"Malformed track data response response: missing `tracks`" );

        auto ret = tracksIt->get<std::vector<std::unique_ptr<const WebApi_Track>>>();
        trackCache_.CacheObjects( ret );
    }
}

std::unique_ptr<const sptf::WebApi_Track>
WebApi_Backend::GetTrack( const std::string& trackId, abort_callback& abort, bool useRelink )
{
    // don't want to cache relinked tracks

    if ( auto trackOpt = trackCache_.GetObjectFromCache( trackId );
         !useRelink && trackOpt )
    {
        return std::unique_ptr<sptf::WebApi_Track>( std::move( *trackOpt ) );
    }
    else
    {
        web::uri_builder builder;
        builder
            .append_path( L"tracks" )
            .append_path( qwr::unicode::ToWide( trackId ) );
        if ( useRelink )
        {
            if ( const auto countryOpt = GetUser( abort )->country;
                 countryOpt )
            {
                builder.append_query( L"market", qwr::unicode::ToWide( *countryOpt ) );
            }
        }

        const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
        auto ret = responseJson.get<std::unique_ptr<WebApi_Track>>();

        if ( !useRelink )
        {
            trackCache_.CacheObject( *ret );
        }
        return std::unique_ptr<sptf::WebApi_Track>( std::move( ret ) );
    }
}

std::vector<std::unique_ptr<const WebApi_Track>>
WebApi_Backend::GetTracks( nonstd::span<const std::string> trackIds, abort_callback& abort )
{
    RefreshCacheForTracks( trackIds, abort );

    return trackIds | ranges::views::transform( [&]( const auto& id ) -> std::unique_ptr<const WebApi_Track> {
               assert( trackCache_.IsCached( id ) );
               return std::move( *trackCache_.GetObjectFromCache( id ) );
           } )
           | ranges::to_vector;
}

std::tuple<
    std::vector<std::unique_ptr<const WebApi_Track>>,
    std::vector<std::unique_ptr<const WebApi_LocalTrack>>>
WebApi_Backend::GetTracksFromPlaylist( const std::string& playlistId, abort_callback& abort )
{
    constexpr size_t kMaxItemsPerRequest = 100;

    auto requestUri = [&] {
        web::uri_builder builder;
        builder
            .append_path( fmt::format( L"playlists/{}/tracks", qwr::unicode::ToWide( playlistId ) ) )
            .append_query( L"limit", kMaxItemsPerRequest, false );

        return builder.to_uri();
    }();

    std::vector<std::unique_ptr<const WebApi_Track>> tracks;
    std::vector<std::unique_ptr<const WebApi_LocalTrack>> localTracks;
    while ( true )
    {
        const auto responseJson = GetJsonResponse( requestUri, abort );
        const auto pPagingObject = responseJson.get<std::unique_ptr<const WebApi_PagingObject>>();

        auto playlistTracks = pPagingObject->items.get<std::vector<std::unique_ptr<WebApi_PlaylistTrack>>>();
        for ( auto& playlistTrack: playlistTracks )
        {
            std::visit( [&]( auto&& arg ) {
                using T = std::decay_t<decltype( arg )>;
                if constexpr ( std::is_same_v<T, WebApi_Track> )
                {
                    tracks.emplace_back( std::make_unique<T>( std::move( arg ) ) );
                }
                else if constexpr ( std::is_same_v<T, WebApi_LocalTrack> )
                {
                    localTracks.emplace_back( std::make_unique<T>( std::move( arg ) ) );
                }
                else
                {
                    static_assert( qwr::always_false_v<T>, "non-exhaustive visitor!" );
                }
            },
                        *playlistTrack->track );
        }

        if ( !pPagingObject->next )
        {
            break;
        }

        requestUri = *pPagingObject->next;
    }

    trackCache_.CacheObjects( tracks );
    return { std::move( tracks ), std::move( localTracks ) };
}

std::vector<std::unique_ptr<const sptf::WebApi_Track>>
WebApi_Backend::GetTracksFromAlbum( const std::string& albumId, abort_callback& abort )
{
    std::shared_ptr<WebApi_Album_Simplified> album;
    web::uri requestUri;
    std::vector<std::unique_ptr<WebApi_Track_Simplified>> ret;
    while ( true )
    {
        const auto responseJson = [&] {
            if ( requestUri.is_empty() )
            { // first paging object (and uri) is retrieved from album
                web::uri_builder builder;
                builder.append_path( fmt::format( L"albums/{}", qwr::unicode::ToWide( albumId ) ) );

                const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
                responseJson.get_to( album );

                const auto tracksIt = responseJson.find( "tracks" );
                qwr::QwrException::ExpectTrue( responseJson.cend() != tracksIt,
                                               L"Malformed track data response: missing `tracks`" );

                return *tracksIt;
            }
            else
            {
                return GetJsonResponse( requestUri, abort );
            }
        }();

        const auto pPagingObject = responseJson.get<std::unique_ptr<const WebApi_PagingObject>>();

        auto newData = pPagingObject->items.get<std::vector<std::unique_ptr<WebApi_Track_Simplified>>>();
        ret.insert( ret.end(), make_move_iterator( newData.begin() ), make_move_iterator( newData.end() ) );

        if ( !pPagingObject->next )
        {
            break;
        }

        requestUri = *pPagingObject->next;
    }

    auto newRet = ranges::views::transform( ret, [&]( auto&& elem ) {
                      return std::make_unique<const WebApi_Track>( std::move( elem ), album );
                  } )
                  | ranges::to_vector;
    trackCache_.CacheObjects( newRet );
    return newRet;
}

std::vector<std::unique_ptr<const WebApi_Track>>
WebApi_Backend::GetTopTracksForArtist( const std::string& artistId, abort_callback& abort )
{
    const auto countryOpt = GetUser( abort )->country;
    qwr::QwrException::ExpectTrue( countryOpt.has_value(),
                                   "Adding artist top tracks requires `user-read-private` permission.\n"
                                   "Re-login to update your permission scope." );

    web::uri_builder builder;
    builder
        .append_path( L"artists" )
        .append_path( qwr::unicode::ToWide( artistId ) )
        .append_path( L"top-tracks" )
        .append_query( L"market", qwr::unicode::ToWide( *countryOpt ) );

    const auto responseJson = GetJsonResponse( builder.to_uri(), abort );

    const auto tracksIt = responseJson.find( "tracks" );
    qwr::QwrException::ExpectTrue( responseJson.cend() != tracksIt,
                                   L"Malformed track data response response: missing `tracks`" );

    auto ret = tracksIt->get<std::vector<std::unique_ptr<const WebApi_Track>>>();
    trackCache_.CacheObjects( ret );
    return ret;
}

std::vector<std::unordered_multimap<std::string, std::string>>
WebApi_Backend::GetMetaForTracks( nonstd::span<const std::unique_ptr<const WebApi_Track>> tracks )
{
    std::vector<std::unordered_multimap<std::string, std::string>> ret;
    for ( const auto& track: tracks )
    {
        auto& curMap = ret.emplace_back();

        // This length will be overriden during playback
        curMap.emplace( "SPTF_LENGTH", fmt::format( "{}", track->duration_ms ) );

        curMap.emplace( "TITLE", track->name );
        curMap.emplace( "TRACKNUMBER", fmt::format( "{}", track->track_number ) );
        curMap.emplace( "DISCNUMBER", fmt::format( "{}", track->disc_number ) );

        for ( const auto& artist: track->artists )
        {
            curMap.emplace( "ARTIST", artist->name );
        }

        const auto& album = track->album;
        curMap.emplace( "ALBUM", album->name );
        curMap.emplace( "DATE", album->release_date );

        for ( const auto& artist: album->artists )
        {
            curMap.emplace( "ALBUM ARTIST", artist->name );
        }
    }

    // TODO: cache data (<https://fy.3dyd.com/help/titleformatting/#Metadata_length_limitations> might be useful)
    // TODO: check webapi_objects for other fields

    return ret;
}

void WebApi_Backend::RefreshCacheForArtists( nonstd::span<const std::string> artistIds, abort_callback& abort )
{
    // remove duplicates
    const auto uniqueIds = artistIds | ranges::to<std::unordered_set<std::string>>;

    for ( const auto& idsChunk:
          uniqueIds
              | ranges::views::remove_if( [&]( const auto& id ) { return artistCache_.IsCached( id ); } )
              | ranges::views::chunk( 50 ) )
    {
        const auto idsStr = qwr::unicode::ToWide( qwr::string::Join( idsChunk | ranges::to_vector, ',' ) );

        web::uri_builder builder;
        builder
            .append_path( L"artists" )
            .append_query( L"ids", idsStr );

        const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
        const auto artistsIt = responseJson.find( "artists" );
        qwr::QwrException::ExpectTrue( responseJson.cend() != artistsIt,
                                       L"Malformed track data response response: missing `artists`" );

        auto ret = artistsIt->get<std::vector<std::unique_ptr<const WebApi_Artist>>>();
        artistCache_.CacheObjects( ret );
    }
}

std::unique_ptr<const WebApi_Artist>
WebApi_Backend::GetArtist( const std::string& artistId, abort_callback& abort )
{
    if ( auto objectOpt = artistCache_.GetObjectFromCache( artistId );
         objectOpt )
    {
        return std::unique_ptr<const WebApi_Artist>( std::move( *objectOpt ) );
    }
    else
    {
        web::uri_builder builder;
        builder
            .append_path( L"artists" )
            .append_path( qwr::unicode::ToWide( artistId ) );

        const auto responseJson = GetJsonResponse( builder.to_uri(), abort );
        auto ret = responseJson.get<std::unique_ptr<const WebApi_Artist>>();
        artistCache_.CacheObject( *ret );
        return std::unique_ptr<const WebApi_Artist>( std::move( ret ) );
    }
}

fs::path WebApi_Backend::GetAlbumImage( const std::string& albumId, const std::string& imgUrl, abort_callback& abort )
{
    return albumImageCache_.GetImage( albumId, imgUrl, abort );
}

fs::path WebApi_Backend::GetArtistImage( const std::string& artistId, const std::string& imgUrl, abort_callback& abort )
{
    return artistImageCache_.GetImage( artistId, imgUrl, abort );
}

web::http::client::http_client_config WebApi_Backend::GetClientConfig()
{
    const auto proxyUrl = qwr::unicode::ToWide( sptf::config::advanced::network_proxy.GetValue() );
    const auto proxyUsername = qwr::unicode::ToWide( sptf::config::advanced::network_proxy_username.GetValue() );
    const auto proxyPassword = qwr::unicode::ToWide( sptf::config::advanced::network_proxy_password.GetValue() );

    web::http::client::http_client_config config;
    if ( !proxyUrl.empty() )
    {
        web::web_proxy proxy( web::uri::encode_uri( proxyUrl ) );
        if ( !proxyUsername.empty() && !proxyPassword.empty() )
        {
            proxy.set_credentials( web::credentials{ proxyUsername, proxyPassword } );
        }

        config.set_proxy( std::move( proxy ) );
    }

    return config;
}

nlohmann::json WebApi_Backend::GetJsonResponse( const web::uri& requestUri, abort_callback& abort )
{
    return ParseResponse( GetResponse( requestUri, abort ) );
}

web::http::http_response WebApi_Backend::GetResponse( const web::uri& requestUri, abort_callback& abort )
{
    const auto adjustedRequestUri = [&] {
        const auto uriStr = requestUri.to_string();
        const auto baseUriStr = client_.base_uri().to_string();
        if ( uriStr._Starts_with( baseUriStr ) )
        {
            return web::uri{ uriStr.data() + baseUriStr.size() };
        }
        else
        {
            return requestUri;
        }
    }();

    if ( shouldLogWebApiRequest_ )
    {
        FB2K_console_formatter() << SPTF_UNDERSCORE_NAME " (debug): request:\n"
                                 << qwr::unicode::ToU8( adjustedRequestUri.to_string() );
    }

    web::http::http_request req( web::http::methods::GET );
    req.headers().add( L"Authorization", fmt::format( L"Bearer {}", pAuth_->GetAccessToken( abort ) ) );
    req.headers().add( L"Accept", L"application/json" );
    req.headers().set_content_type( L"application/json" );

    req.set_request_uri( adjustedRequestUri );

    rpsLimiter_.WaitForRequestAvailability( abort );
    qwr::QwrException::ExpectTrue( !abort.is_aborting(), "Abort was signaled, canceling request..." );

    auto ctsToken = cts_.get_token();
    auto localCts = Concurrency::cancellation_token_source::create_linked_source( ctsToken );
    const auto abortableScope = abortManager_.GetAbortableScope( [&localCts] { localCts.cancel(); }, abort );

    web::http::http_response response;
    for ( size_t i = 0; i < 3; ++i )
    {
        response = client_.request( req, localCts.get_token() ).get();
        if ( response.status_code() != 429 )
        {
            break;
        }

        const auto it = response.headers().find( L"Retry-After" );
        qwr::QwrException::ExpectTrue( it != response.headers().end(), "Request failed with 429 error, but does not contain a `Retry-After` header" );

        const auto& [_, retryHeader] = *it;
        const auto retryInMsOpt = qwr::string::GetNumber<uint32_t>( qwr::unicode::ToU8( retryHeader ) );
        qwr::QwrException::ExpectTrue( retryInMsOpt.has_value(), "Request failed with 429 error, but does not contain a valid number in `Retry-After` header" );

        const auto retryIn = std::chrono::milliseconds( *retryInMsOpt ) + std::chrono::seconds( 1 );
        FB2K_console_formatter() << SPTF_UNDERSCORE_NAME " (error):\n"
                                 << fmt::format( L"Rate limit reached: retrying in {} ms", retryIn.count() );
        if ( !SleepFor( retryIn, abort ) )
        {
            break;
        }
    }

    if ( response.status_code() == 429 )
    {
        FB2K_console_formatter() << SPTF_UNDERSCORE_NAME " (error):\n"
                                 << fmt::format( L"Rate limit reached: retry failed" );
    }

    return response;
}

nlohmann::json WebApi_Backend::ParseResponse( const web::http::http_response& response )
{
    if ( response.status_code() != 200 )
    {
        throw qwr::QwrException( L"{}: {}\n"
                                 L"Additional data: {}\n",
                                 (int)response.status_code(),
                                 response.reason_phrase(),
                                 [&]() -> std::wstring {
                                     try
                                     {
                                         const auto responseJson = nlohmann::json::parse( response.extract_string().get() );
                                         return qwr::unicode::ToWide( responseJson.dump( 2 ) );
                                     }
                                     catch ( ... )
                                     {
                                         return response.to_string();
                                     }
                                 }() );
    }

    const auto responseJson = nlohmann::json::parse( response.extract_string().get() );
    if ( shouldLogWebApiResponse_ )
    {
        FB2K_console_formatter() << SPTF_UNDERSCORE_NAME " (debug): response:\n"
                                 << responseJson.dump( 2 );
    }
    qwr::QwrException::ExpectTrue( responseJson.is_object(),
                                   L"Malformed track data response response: json is not an object" );

    return responseJson;
}

} // namespace sptf
