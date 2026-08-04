#include <mydb/db.hpp>
#include <mydb/network/protocol.hpp>
#include <mydb/config.hpp>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCKET(s) closesocket(s)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define INVALID_SOCK -1
#define CLOSE_SOCKET(s) close(s)
#endif

namespace {
    std::atomic<bool> g_running{true};
    
    void SignalHandler(int signal) {
        spdlog::info("Received signal {}, shutting down...", signal);
        g_running.store(false);
    }
}

void PrintBanner() {
    std::cout << R"(
  __  __       ____  ____  
 |  \/  |_   _|  _ \| __ ) 
 | |\/| | | | | | | |  _ \ 
 | |  | | |_| | |_| | |_) |
 |_|  |_|\__, |____/|____/ 
         |___/             
    )" << std::endl;
    std::cout << "MyDB v" << mydb::kVersion << " - High-Performance LSM-Tree Database" << std::endl;
    std::cout << "==========================================" << std::endl;
}

void PrintUsage(const char* program) {
    std::cout << "Usage: " << program << " [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --path <dir>      Database directory (default: ./mydb_data)" << std::endl;
    std::cout << "  --port <port>     Server port (default: 6379)" << std::endl;
    std::cout << "  --debug           Enable debug logging" << std::endl;
    std::cout << "  --help            Show this help message" << std::endl;
}

void HandleClient(socket_t client_sock, mydb::Database* db) {
    spdlog::debug("Handling client connection");
    
    std::vector<char> buffer(4096);
    
    while (g_running.load()) {

#ifdef _WIN32
        int received = recv(client_sock, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        ssize_t received = recv(client_sock, buffer.data(), buffer.size(), 0);
#endif

        if (received <= 0) {
            spdlog::debug("Client disconnected");
            break;
        }

        spdlog::debug("Received {} bytes", received);

        auto req_result = mydb::Protocol::ParseRequest(
            mydb::Slice(buffer.data(), static_cast<size_t>(received))
        );

        std::vector<char> response;
        
        if (!req_result.ok()) {
            spdlog::warn("Invalid request: {}", req_result.status().message());
            response = mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                mydb::response::kInvalidRequest, 
                req_result.status().message()
            });
        } else {
            const auto& req = req_result.value();
            
            response = std::visit([db](auto&& r) -> std::vector<char> {
                using T = std::decay_t<decltype(r)>;
                
                if constexpr (std::is_same_v<T, mydb::PutRequest>) {
                    mydb::Status s = db->Put(r.key, r.value);
                    if (s.ok()) {
                        return mydb::Protocol::EncodeResponse(mydb::OkResponse{"OK"});
                    }
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kError, s.message()
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::GetRequest>) {
                    auto result = db->Get(r.key);
                    if (result.ok()) {
                        std::string val = result.value();
                        
                        if (r.field.has_value()) {
                            std::string field_name = r.field.value();
                             spdlog::debug("Attempting extraction. val='{}', field='{}'", val, field_name);
                            
                            size_t pos = val.find("\"" + field_name + "\"");
                            if (pos == std::string::npos) {
                                pos = val.find(field_name + ":");
                            }
                            if (pos == std::string::npos) {
                                pos = val.find(field_name);
                            }
                            
                            if (pos != std::string::npos) {
                                size_t colon = val.find(":", pos);
                                if (colon != std::string::npos) {
                                    size_t start = colon + 1;
                                    while (start < val.size() && isspace(val[start])) start++;
                                    
                                    size_t end = val.find_first_of(",;}", start);
                                    if (end == std::string::npos) end = val.size();
                                    
                                    std::string extracted = val.substr(start, end - start);
                                    
                                    while (!extracted.empty() && isspace(extracted.back())) extracted.pop_back();
                                    
                                    if (extracted.size() >= 2 && extracted.front() == '"' && extracted.back() == '"') {
                                        extracted = extracted.substr(1, extracted.size() - 2);
                                    }
                                    
                                    return mydb::Protocol::EncodeResponse(mydb::ValueResponse{extracted});
                                }
                            }
                            return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                                mydb::response::kNotFound, "Field not found"
                            });
                        }
                        
                        return mydb::Protocol::EncodeResponse(mydb::ValueResponse{val});
                    }
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kNotFound, "Not found"
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::DeleteRequest>) {
                    mydb::Status s = db->Delete(r.key);
                    if (s.ok()) {
                        return mydb::Protocol::EncodeResponse(mydb::OkResponse{"OK"});
                    }
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kError, s.message()
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::PingRequest>) {
                    return mydb::Protocol::EncodeResponse(mydb::OkResponse{"PONG"});
                }
                else if constexpr (std::is_same_v<T, mydb::StatusRequest>) {
                    auto stats = db->GetStats();
                    return mydb::Protocol::EncodeResponse(mydb::StatusResponse{
                        stats.num_entries,
                        stats.memtable_size,
                        stats.num_sstables,
                        db->GetVersion()
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::FlushRequest>) {
                    mydb::Status s = db->Flush();
                    if (s.ok()) {
                        return mydb::Protocol::EncodeResponse(mydb::OkResponse{"Flushed"});
                    }
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kError, s.message()
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::CompactRequest>) {
                    mydb::Status s = db->CompactLevel(r.level);
                    if (s.ok()) {
                        return mydb::Protocol::EncodeResponse(mydb::OkResponse{"Compacted"});
                    }
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kError, s.message()
                    });
                }
                else if constexpr (std::is_same_v<T, mydb::ExecPythonRequest>) {
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kNotFound, "Python not supported in this build"
                    });
                }
                else {
                    return mydb::Protocol::EncodeResponse(mydb::ErrorResponse{
                        mydb::response::kInvalidRequest, "Unknown request type"
                    });
                }
            }, req);
        }

#ifdef _WIN32
        int sent = send(client_sock, response.data(), static_cast<int>(response.size()), 0);
#else
        ssize_t sent = send(client_sock, response.data(), response.size(), 0);
#endif
        
        if (sent <= 0) {
            spdlog::debug("Failed to send response");
            break;
        }
        
        spdlog::debug("Sent {} bytes", sent);
    }
    
    CLOSE_SOCKET(client_sock);
}

int main(int argc, char* argv[]) {
    try {
    std::cout << "Starting MyDB Server..." << std::endl;
    
    mydb::Options options;
    options.db_path = "./mydb_data";
    options.port = mydb::kDefaultPort;
    options.create_if_missing = true;
    
    bool debug = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        }
        else if (arg == "--path" && i + 1 < argc) {
            options.db_path = argv[++i];
        }
        else if (arg == "--port" && i + 1 < argc) {
            options.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        else if (arg == "--debug") {
            debug = true;
        }
        else {
            std::cerr << "Unknown option: " << arg << std::endl;
            PrintUsage(argv[0]);
            return 1;
        }
    }
    
    std::cout << "Initializing logger..." << std::endl;
    
    try {
        auto console = spdlog::stdout_color_mt("mydb");
        spdlog::set_default_logger(console);
        spdlog::set_level(debug ? spdlog::level::debug : spdlog::level::info);
        spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    } catch (const std::exception& e) {
        std::cerr << "Logger init failed: " << e.what() << std::endl;
    }
    
    PrintBanner();
    
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        spdlog::error("WSAStartup failed");
        return 1;
    }
#endif
    
    spdlog::info("Opening database: {}", options.db_path);
    
    auto db_result = mydb::Database::Open(options);
    if (!db_result.ok()) {
        spdlog::error("Failed to open database: {}", db_result.status().ToString());
        return 1;
    }
    
    auto& db = db_result.value();
    spdlog::info("Database opened successfully");
    
    socket_t server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock == INVALID_SOCK) {
        spdlog::error("Failed to create socket");
        return 1;
    }
    
    int opt = 1;
#ifdef _WIN32
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, 
               reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(options.port);
    
    if (bind(server_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        spdlog::error("Failed to bind to port {}", options.port);
        CLOSE_SOCKET(server_sock); 
        return 1;

    }
    
    if (listen(server_sock, 128) < 0) {
        spdlog::error("Failed to listen");
        CLOSE_SOCKET(server_sock);
        return 1;
    }
    
    spdlog::info("Server listening on port {}", options.port);
    spdlog::info("Press Ctrl+C to stop");
    
    std::vector<std::thread> client_threads;
    
    while (g_running.load()) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_sock, &read_fds);
        
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        
        int select_result = select(static_cast<int>(server_sock) + 1, &read_fds, nullptr, nullptr, &timeout);
        
        if (select_result <= 0) {
            continue;
        }
        
        sockaddr_in client_addr{};

        socklen_t client_len = sizeof(client_addr);
        
        socket_t client_sock = accept(server_sock, 
                                       reinterpret_cast<sockaddr*>(&client_addr), 
                                       &client_len);
        
        if (client_sock == INVALID_SOCK) {
            spdlog::debug("Accept failed");
            continue;
        }
        
        spdlog::info("New connection accepted from client");
        
        client_threads.emplace_back([client_sock, &db]() {
            HandleClient(client_sock, db.get());
        });
    }
    
    spdlog::info("Shutting down...");
    CLOSE_SOCKET(server_sock);
    
    for (auto& t : client_threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    auto stats = db->GetStats();
    spdlog::info("Final stats: {} entries, {} SSTables, {} bytes on disk",
                 stats.num_entries, stats.num_sstables, stats.disk_usage);
    
    spdlog::info("Goodbye!");
    return 0;
    
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;
        return 1;
    }
}