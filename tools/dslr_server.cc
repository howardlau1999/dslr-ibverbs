// dslr_server: hosts one node's lock table and bootstraps RDMA queue pairs for clients.
//
//   dslr_server --device rxe0 --locks 1000000 --port 7777
//
// After start-up the server's CPU is idle: all lock operations are one-sided RDMA verbs executed
// by the NIC. Press Ctrl-C to stop. With --dump-interval the server periodically prints the
// lock objects that are currently not idle, which is handy when debugging a protocol issue.

#include <atomic>
#include <csignal>
#include <iostream>
#include <thread>

#include "dslr/lock_table_server.h"
#include "dslr/rdma/error.h"

#include "args.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) {
  g_stop.store(true);
}

void usage() {
  std::cout
      << "Usage: dslr_server [options]\n"
         "  --device NAME         ibverbs device (default: first device)\n"
         "  --ib-port N           physical port (default: 1)\n"
         "  --gid-index N         GID table index for RoCE (default: auto)\n"
         "  --locks N             number of lock objects (default: 1048576)\n"
         "  --bind ADDR           bind address for the bootstrap listener (default: 0.0.0.0)\n"
         "  --port N              bootstrap TCP port (default: 7777)\n"
         "  --dump-interval DUR   print busy lock objects every DUR (e.g. 1s); default: off\n";
}

}  // namespace

int main(int argc, char** argv) {
  using dslr::tools::Args;
  Args args(argc, argv);
  if (args.flag("help")) {
    usage();
    return 0;
  }

  dslr::ServerOptions options;
  options.device.name = args.get("device", "");
  options.device.port = args.number<uint8_t>("ib-port", 1);
  if (args.has("gid-index")) {
    options.device.gid_index = args.number<uint32_t>("gid-index", 0);
  }
  options.lock_count = args.number<uint32_t>("locks", 1u << 20);
  options.bind_address = args.get("bind", "0.0.0.0");
  options.port = args.number<uint16_t>("port", 7777);
  const auto dump_interval = args.duration("dump-interval", std::chrono::microseconds{0});
  options.on_handshake_error = [](const std::string& error) {
    std::cerr << "dslr_server: client handshake failed: " << error << std::endl;
  };

  try {
    dslr::LockTableServer server(options);
    server.start();
    const dslr::rdma::Device& dev = server.device();
    std::cout << "dslr_server: device " << dev.name() << " port " << int(dev.port())
              << (dev.needs_global_routing()
                      ? " (RoCE, gid index " + std::to_string(dev.gid_index()) + ")"
                      : " (InfiniBand, lid " + std::to_string(dev.lid()) + ")")
              << "\n"
              << "dslr_server: hosting " << server.lock_count() << " lock objects ("
              << (server.lock_count() * sizeof(uint64_t)) / 1024 << " KiB), listening on "
              << options.bind_address << ":" << server.port() << std::endl;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    auto next_dump = std::chrono::steady_clock::now() + dump_interval;
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (dump_interval.count() > 0 && std::chrono::steady_clock::now() >= next_dump) {
        next_dump += dump_interval;
        std::cout << "clients=" << server.client_count() << " busy locks:";
        unsigned shown = 0;
        for (uint32_t i = 0; i < server.lock_count() && shown < 16; ++i) {
          const dslr::LockWord w = server.peek(i);
          if (w.nX != w.maxX || w.nS != w.maxS) {
            std::cout << " [" << i << "]" << dslr::to_string(w);
            ++shown;
          }
        }
        std::cout << (shown == 0 ? " none" : "") << std::endl;
      }
    }
    std::cout << "dslr_server: shutting down" << std::endl;
    server.stop();
  } catch (const std::exception& e) {
    std::cerr << "dslr_server: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
