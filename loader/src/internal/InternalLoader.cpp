#include "InternalLoader.hpp"

#include "InternalMod.hpp"
#include "resources.hpp"

#include <Geode/loader/Loader.hpp>
#include <Geode/loader/Log.hpp>
#include <Geode/loader/Dirs.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <fmt/format.h>
#include <hash.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

ListenerResult ResourceDownloadFilter::handle(
    std::function<Callback> fn,
    ResourceDownloadEvent* event
) {
    fn(event);
    return ListenerResult::Propagate;
}

ResourceDownloadFilter::ResourceDownloadFilter() {}

InternalLoader::InternalLoader() : Loader() {}

InternalLoader::~InternalLoader() {
    this->closePlatformConsole();
}

InternalLoader* InternalLoader::get() {
    static auto g_geode = new InternalLoader;
    return g_geode;
}

bool InternalLoader::setup() {
    log::log(Severity::Debug, InternalMod::get(), "Set up internal mod representation");
    log::log(Severity::Debug, InternalMod::get(), "Loading hooks... ");

    if (!this->loadHooks()) {
        log::log(
            Severity::Error, InternalMod::get(),
            "There were errors loading some hooks, see console for details"
        );
    }

    log::log(Severity::Debug, InternalMod::get(), "Loaded hooks");

    log::log(Severity::Debug, InternalMod::get(), "Setting up IPC...");

    this->setupIPC();

    return true;
}

void InternalLoader::queueInGDThread(ScheduledFunction func) {
    std::lock_guard<std::mutex> lock(m_gdThreadMutex);
    m_gdThreadQueue.push_back(func);
}

void InternalLoader::executeGDThreadQueue() {
    // copy queue to avoid locking mutex if someone is
    // running addToGDThread inside their function
    m_gdThreadMutex.lock();
    auto queue = m_gdThreadQueue;
    m_gdThreadQueue.clear();
    m_gdThreadMutex.unlock();

    // call queue
    for (auto const& func : queue) {
        func();
    }
}

void InternalLoader::logConsoleMessage(std::string const& msg) {
    if (m_platformConsoleOpen) {
        // TODO: make flushing optional
        std::cout << msg << '\n' << std::flush;
    }
}

bool InternalLoader::platformConsoleOpen() const {
    return m_platformConsoleOpen;
}

bool InternalLoader::shownInfoAlert(std::string const& key) {
    if (m_shownInfoAlerts.count(key)) {
        return true;
    }
    m_shownInfoAlerts.insert(key);
    return false;
}

void InternalLoader::saveInfoAlerts(nlohmann::json& json) {
    json["alerts"] = m_shownInfoAlerts;
}

void InternalLoader::loadInfoAlerts(nlohmann::json& json) {
    m_shownInfoAlerts = json["alerts"].get<std::unordered_set<std::string>>();
}

void InternalLoader::downloadLoaderResources() {
    auto version = this->getVersion().toString();
    auto tempResourcesZip = dirs::getTempDir() / "new.zip";
    auto resourcesDir = dirs::getGeodeResourcesDir() / InternalMod::get()->getID();

    web::AsyncWebRequest()
        .join("update-geode-loader-resources")
        .fetch(fmt::format(
            "https://github.com/geode-sdk/geode/releases/download/{}/resources.zip", version
        ))
        .into(tempResourcesZip)
        .then([tempResourcesZip, resourcesDir](auto) {
            // unzip resources zip
            auto unzip = file::Unzip::intoDir(tempResourcesZip, resourcesDir, true);
            if (!unzip) {
                return ResourceDownloadEvent(
                    UpdateError("Unable to unzip new resources: " + unzip.unwrapErr())
                ).post();
            }
            ResourceDownloadEvent(UpdateFinished()).post();
        })
        .expect([](std::string const& info) {
            ResourceDownloadEvent(
                UpdateError("Unable to download resources: " + info)
            ).post();
        })
        .progress([](auto&, double now, double total) {
            ResourceDownloadEvent(
                UpdateProgress(
                    static_cast<uint8_t>(now / total * 100.0),
                    "Downloading resources"
                )
            ).post();
        });
}

bool InternalLoader::verifyLoaderResources() {
    static std::optional<bool> CACHED = std::nullopt;
    if (CACHED.has_value()) {
        return CACHED.value();
    }

    // geode/resources/geode.loader
    auto resourcesDir = dirs::getGeodeResourcesDir() / InternalMod::get()->getID();

    // if the resources dir doesn't exist, then it's probably incorrect
    if (!(
        ghc::filesystem::exists(resourcesDir) &&
        ghc::filesystem::is_directory(resourcesDir)
    )) {
        this->downloadLoaderResources();
        return false;
    }

    // make sure every file was covered
    size_t coverage = 0;

    // verify hashes
    for (auto& file : ghc::filesystem::directory_iterator(resourcesDir)) {
        auto name = file.path().filename().string();
        // skip unknown files
        if (!LOADER_RESOURCE_HASHES.count(name)) {
            continue;
        }
        // verify hash
        auto hash = calculateSHA256(file.path());
        if (hash != LOADER_RESOURCE_HASHES.at(name)) {
            log::debug(
                "compare {} {} {}", file.path().string(), hash, LOADER_RESOURCE_HASHES.at(name)
            );
            this->downloadLoaderResources();
            return false;
        }
        coverage += 1;
    }

    // make sure every file was found
    if (coverage != LOADER_RESOURCE_HASHES.size()) {
        this->downloadLoaderResources();
        return false;
    }

    return true;
}
