// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// A gRPC client for the functional tests (test/functional/interface_ecash_grpc.py), built on gRPC's
// own C++ library (grpc++), when it is installed: an implementation of gRPC other than the server's,
// as the sidechains' tonic is. It knows no message types: requests and replies are hex.
//
//   flush-grpc-client HOST:PORT /package.Service/Method [--count N] [--timeout SECONDS] [--header NAME:VALUE]...
//
// Reads the request messages from stdin, one per line in hex (an empty line: an empty message), and
// prints each reply as "MSG <hex>", then "STATUS <code> <message>". With --count it cancels the call
// after N replies (a stream that does not end). Each line is flushed as it is printed.

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string ToHex(const std::string& bytes)
{
    static const char* digits{"0123456789abcdef"};
    std::string out;
    for (const unsigned char c : bytes) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 15]);
    }
    return out;
}

bool FromHex(const std::string& hex, std::string& out)
{
    if (hex.size() % 2) return false;
    const auto value{[](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }};
    for (size_t i{0}; i < hex.size(); i += 2) {
        const int hi{value(hex[i])}, lo{value(hex[i + 1])};
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
    }
    return true;
}

bool Wait(grpc::CompletionQueue& cq, void* tag)
{
    void* got;
    bool ok;
    if (!cq.Next(&got, &ok)) return false;
    if (got != tag) {
        std::cerr << "unexpected completion" << std::endl;
        std::exit(2);
    }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::cerr << "usage: flush-grpc-client HOST:PORT /package.Service/Method [--count N] [--timeout SECONDS]" << std::endl;
        return 2;
    }
    const std::string target{argv[1]}, method{argv[2]};
    long count{-1};
    double timeout{60};
    std::vector<std::pair<std::string, std::string>> headers;
    for (int i{3}; i + 1 < argc; i += 2) {
        const std::string flag{argv[i]};
        if (flag == "--count") count = std::stol(argv[i + 1]);
        else if (flag == "--timeout") timeout = std::stod(argv[i + 1]);
        else if (flag == "--header") {
            const std::string h{argv[i + 1]};
            const auto colon{h.find(':')};
            if (colon == std::string::npos) return 2;
            headers.emplace_back(h.substr(0, colon), h.substr(colon + 1));
        }
    }
    std::vector<std::string> requests;
    for (std::string line; std::getline(std::cin, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::string bytes;
        if (!FromHex(line, bytes)) {
            std::cerr << "not hex: " << line << std::endl;
            return 2;
        }
        requests.push_back(bytes);
    }

    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(-1);
    const auto channel{grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args)};
    grpc::GenericStub stub{channel};
    grpc::ClientContext context;
    for (const auto& [name, value] : headers) context.AddMetadata(name, value);
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(static_cast<long>(timeout * 1000)));
    grpc::CompletionQueue cq;
    const auto call{stub.PrepareCall(&context, method, &cq)};
    int tag_start, tag_write, tag_done, tag_read, tag_finish;
    call->StartCall(&tag_start);
    bool ok{Wait(cq, &tag_start)};
    for (const std::string& r : requests) {
        if (!ok) break;
        grpc::Slice slice{r};
        grpc::ByteBuffer buffer{&slice, 1};
        call->Write(buffer, &tag_write);
        ok = Wait(cq, &tag_write);
    }
    if (ok) {
        call->WritesDone(&tag_done);
        Wait(cq, &tag_done);
    }
    long received{0};
    for (;;) {
        grpc::ByteBuffer reply;
        call->Read(&reply, &tag_read);
        if (!Wait(cq, &tag_read)) break;
        std::vector<grpc::Slice> slices;
        (void)reply.Dump(&slices);
        std::string bytes;
        for (const auto& s : slices) bytes.append(reinterpret_cast<const char*>(s.begin()), s.size());
        std::printf("MSG %s\n", ToHex(bytes).c_str());
        std::fflush(stdout);
        if (count >= 0 && ++received >= count) {
            context.TryCancel();
            break;
        }
    }
    grpc::Status status;
    call->Finish(&status, &tag_finish);
    Wait(cq, &tag_finish);
    std::printf("STATUS %d %s\n", static_cast<int>(status.error_code()), status.error_message().c_str());
    std::fflush(stdout);
    return 0;
}
