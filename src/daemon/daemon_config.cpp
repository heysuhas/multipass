/*
 * Copyright (C) 2017-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "daemon_config.h"

#include "custom_image_host.h"
#include "ubuntu_image_host.h"

#include <multipass/client_cert_store.h>
#include <multipass/logging/log.h>
#include <multipass/logging/standard_logger.h>
#include <multipass/name_generator.h>
#include <multipass/platform.h>
#include <multipass/ssh/openssh_key_provider.h>
#include <multipass/ssl_cert_provider.h>
#include <multipass/standard_paths.h>
#include <multipass/utils.h>

#include <QString>
#include <QUrl>

#include <chrono>
#include <memory>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{
constexpr auto manifest_ttl = std::chrono::minutes{5};

std::string server_name_from(const std::string& server_address)
{
    auto tokens = mp::utils::split(server_address, ":");
    const auto server_name = tokens[0];

    if (server_name == "unix")
        return "localhost";
    return server_name;
}

std::unique_ptr<QNetworkProxy> discover_http_proxy()
{
    std::unique_ptr<QNetworkProxy> proxy_ptr{nullptr};

    QString http_proxy{qgetenv("http_proxy")};
    if (http_proxy.isEmpty())
    {
        // Some OS's are case senstive
        http_proxy = qgetenv("HTTP_PROXY");
    }

    if (!http_proxy.isEmpty())
    {
        if (!http_proxy.contains("://"))
        {
            http_proxy.prepend("http://");
        }

        QUrl proxy_url{http_proxy};
        const auto host = proxy_url.host();
        const auto port = proxy_url.port();

        auto network_proxy = QNetworkProxy(QNetworkProxy::HttpProxy, host, static_cast<quint16>(port),
                                           proxy_url.userName(), proxy_url.password());

        QNetworkProxy::setApplicationProxy(network_proxy);

        proxy_ptr = std::make_unique<QNetworkProxy>(network_proxy);
    }

    return proxy_ptr;
}
} // namespace

mp::DaemonConfig::~DaemonConfig()
{
    mpl::set_logger(nullptr);
}

std::unique_ptr<const mp::DaemonConfig> mp::DaemonConfigBuilder::build()
{
    // Install logger as early as possible
    if (logger == nullptr)
        logger = platform::make_logger(verbosity_level);
    // Fallback when platform does not have a logger
    if (logger == nullptr)
        logger = std::make_unique<mpl::StandardLogger>(verbosity_level);

    auto multiplexing_logger = std::make_shared<mpl::MultiplexingLogger>(std::move(logger));
    mpl::set_logger(multiplexing_logger);

    if (cache_directory.isEmpty())
        cache_directory = StandardPaths::instance().writableLocation(StandardPaths::CacheLocation);
    if (data_directory.isEmpty())
        data_directory = StandardPaths::instance().writableLocation(StandardPaths::AppDataLocation);
    if (url_downloader == nullptr)
        url_downloader = std::make_unique<URLDownloader>(cache_directory, std::chrono::seconds{10});
    if (factory == nullptr)
        factory = platform::vm_backend(data_directory);
    if (update_prompt == nullptr)
        update_prompt = platform::make_update_prompt();
    if (image_hosts.empty())
    {
        image_hosts.push_back(std::make_unique<mp::CustomVMImageHost>(url_downloader.get(), manifest_ttl));
        image_hosts.push_back(std::make_unique<mp::UbuntuVMImageHost>(
            std::vector<std::pair<std::string, std::string>>{
                {mp::release_remote, "https://cloud-images.ubuntu.com/releases/"},
                {mp::daily_remote, "https://cloud-images.ubuntu.com/daily/"}},
            url_downloader.get(), manifest_ttl));
    }
    if (vault == nullptr)
    {
        std::vector<VMImageHost*> hosts;
        for (const auto& image : image_hosts)
        {
            hosts.push_back(image.get());
        }

        vault = platform::make_image_vault(
            hosts, url_downloader.get(),
            mp::utils::backend_directory_path(cache_directory, factory->get_backend_directory_name()),
            mp::utils::backend_directory_path(data_directory, factory->get_backend_directory_name()), days_to_expire);
    }
    if (name_generator == nullptr)
        name_generator = mp::make_default_name_generator();
    if (server_address.empty())
        server_address = platform::default_server_address();
    if (ssh_key_provider == nullptr)
        ssh_key_provider = std::make_unique<OpenSSHKeyProvider>(data_directory);
    if (cert_provider == nullptr)
        cert_provider = std::make_unique<mp::SSLCertProvider>(mp::utils::make_dir(data_directory, "certificates"),
                                                              server_name_from(server_address));
    if (client_cert_store == nullptr)
        client_cert_store =
            std::make_unique<mp::ClientCertStore>(mp::utils::make_dir(data_directory, "registered-certs"));
    if (ssh_username.empty())
        ssh_username = "ubuntu";

    if (network_proxy == nullptr)
        network_proxy = discover_http_proxy();

    return std::unique_ptr<const DaemonConfig>(new DaemonConfig{
        std::move(url_downloader), std::move(factory), std::move(image_hosts), std::move(vault),
        std::move(name_generator), std::move(ssh_key_provider), std::move(cert_provider), std::move(client_cert_store),
        std::move(update_prompt), multiplexing_logger, std::move(network_proxy), cache_directory, data_directory,
        server_address, ssh_username, connection_type, image_refresh_timer});
}
