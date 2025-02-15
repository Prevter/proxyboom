#include <Geode/modify/CCHttpClient.hpp>
#include <Geode/utils/web.hpp>

using namespace cocos2d;
using namespace cocos2d::extension;

volatile bool g_proxyChecked = false;
volatile bool g_shouldProxy = false;

namespace settings {
#define STATIC_SETTING(name, type) \
    static type val = (geode::listenForSettingChanges<type>(name, [](auto value) { \
        val = std::move(value); \
    }), geode::Mod::get()->getSettingValue<type>(name));\
    return val

    bool isEnabled() { STATIC_SETTING("enabled", bool); }
    std::string const& getProxyHost() { STATIC_SETTING("proxy-url", std::string); }
    bool disableSSL() { STATIC_SETTING("disable-ssl", bool); }
    bool forceUseProxy() { STATIC_SETTING("force-use-proxy", bool); }

    std::string getProxyUrl() {
        if (disableSSL()) {
            return fmt::format("http://{}", getProxyHost());
        }
        return fmt::format("https://{}", getProxyHost());
    }
}

namespace utils {
    // Checks if we need to enable the proxy (locks the thread until the value is set)
    bool needToProxy() {
        static bool checked = false;
        static bool shouldProxy = false;

        if (!settings::isEnabled()) return false;

        if (!checked) {
            while (!g_proxyChecked) {
                std::this_thread::yield();
            }
            shouldProxy = g_shouldProxy;
        }
        return shouldProxy;
    }

    // Filters only boomlings.com URLs
    bool isBoomlings(std::string_view url) {
        return url.find("://www.boomlings.com") != std::string::npos
            || url.starts_with("www.boomlings.com");
    }

    // Replaces the hostname in the url with the specified host
    std::string replaceHost(std::string_view url, std::string_view host, bool disableSSL = false) {
        auto hostPos = url.find("www.boomlings.com");
        if (hostPos == std::string::npos) return std::string(url);
        if (disableSSL && url.starts_with("https://")) {
            return fmt::format("http://{}{}", host, url.substr(hostPos + 17));
        }
        return fmt::format("{}{}{}", url.substr(0, hostPos), host, url.substr(hostPos + 17));
    }
}

namespace hooks {
    struct ProxiedHttpClient final : geode::Modify<ProxiedHttpClient, CCHttpClient> {
        void send(CCHttpRequest* request) {
            if (utils::isBoomlings(request->_url) && utils::needToProxy()) {
                // geode::log::info("[Cocos] Sending request to proxy server {}", request->_url);
                request->_url = utils::replaceHost(request->_url, settings::getProxyHost(), settings::disableSSL());
            }
            CCHttpClient::send(request);
        }
    };

    using namespace geode::utils;
    web::WebTask WebRequest_send(web::WebRequest* self, std::string_view method, std::string_view url) {
        if (utils::isBoomlings(url) && utils::needToProxy()) {
            // geode::log::info("[Geode] Sending request to proxy server {}", url);
            return self->send(method, utils::replaceHost(url, settings::getProxyHost(), settings::disableSSL()));
        }
        return self->send(method, url);
    }

    $execute {
        auto addr = geode::addresser::getNonVirtual(&web::WebRequest::send);
        auto res = geode::Mod::get()->hook(
            reinterpret_cast<void*>(addr), &WebRequest_send,
            "geode::utils::web::WebRequest::send",
            tulip::hook::TulipConvention::Thiscall
        );
        if (!res) {
            geode::log::error("Failed to hook WebRequest::send!");
        }
    }
}

$on_mod(Loaded) {
    if (settings::forceUseProxy()) {
        g_proxyChecked = true;
        g_shouldProxy = true;
        return;
    }

    using namespace geode::utils::web;
    WebRequest()
        .certVerification(false)
        .get(settings::getProxyUrl())
        .listen([](WebResponse* res) {
            if (res->ok()) {
                geode::log::info("Successfully connected to the proxy server!");
                g_proxyChecked = true;
                g_shouldProxy = true;
            } else {
                geode::log::error("Failed to connect to the proxy server!");
                g_proxyChecked = true;
            }
        });
}