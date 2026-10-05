#pragma once
// Shared bits of the bromux command line.

#include <bromux/client.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bromux::cli {

class Args {
public:
    Args(int argc, char** argv, int start) {
        for (int i = start; i < argc; ++i) v_.emplace_back(argv[i]);
    }
    [[nodiscard]] bool done() const { return i_ >= v_.size(); }
    std::string next() { return done() ? std::string() : v_[i_++]; }
    // The value after option `opt`; marks the parse failed when missing.
    std::string value(const std::string& opt) {
        if (done()) {
            std::fprintf(stderr, "bromux: %s needs a value\n", opt.c_str());
            failed_ = true;
            return {};
        }
        return next();
    }
    [[nodiscard]] bool failed() const { return failed_; }

private:
    std::vector<std::string> v_;
    size_t i_{0};
    bool failed_{false};
};

struct Target {
    ConnectOptions options;
    bool remote{false};
    SshTarget ssh;
};

// Parses target options; anything else goes to `extra` (false = unknown).
bool parse_target(Args& a, Target& t, const std::function<bool(const std::string&)>& extra);
std::unique_ptr<Client> connect_target(const Target& t, bool autostart, std::string* err);

int cmd_attach(Args& a);

}  // namespace bromux::cli
