// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <grpc/standard.h>

#include <algorithm>
#include <stdexcept>

namespace grpcsrv {

namespace {
pb::Message New(const pb::Pool& pool, const std::string& name)
{
    const pb::MessageDesc* desc{pool.FindMessage(name)};
    if (!desc) throw std::logic_error("no message " + name);
    return pb::Message{*desc};
}

void Add(Server& server, const std::string& service, const std::string& method, Handler fn)
{
    if (!server.Register(service, method, std::move(fn))) throw std::logic_error("no method " + service + "/" + method);
}
} // namespace

void RegisterHealth(Server& server, const pb::Pool& pool, const std::vector<std::string>& services)
{
    const auto serving{[services](const std::string& service) { return service.empty() || std::find(services.begin(), services.end(), service) != services.end(); }};
    Add(server, "grpc.health.v1.Health", "Check", [&pool, serving](const pb::Message& req, const std::shared_ptr<Writer>& w) {
        if (!serving(req.GetString("service"))) return w->Finish({Code::NOT_FOUND, "unknown service " + req.GetString("service")});
        pb::Message resp{New(pool, "grpc.health.v1.HealthCheckResponse")};
        resp.SetUInt("status", 1); // SERVING
        w->Write(resp);
        w->Finish({});
    });
    // The status, and the end of the stream: a public server keeps no stream open for nothing.
    Add(server, "grpc.health.v1.Health", "Watch", [&pool, serving](const pb::Message& req, const std::shared_ptr<Writer>& w) {
        pb::Message resp{New(pool, "grpc.health.v1.HealthCheckResponse")};
        resp.SetUInt("status", serving(req.GetString("service")) ? 1 : 3); // SERVING, SERVICE_UNKNOWN
        w->Write(resp);
        w->Finish({});
    });
}

void RegisterReflection(Server& server, const pb::Pool& pool, const std::vector<std::string>& services)
{
    for (const std::string version : {"v1", "v1alpha"}) {
        const std::string pkg{"grpc.reflection." + version};
        Add(server, pkg + ".ServerReflection", "ServerReflectionInfo", [&pool, services, pkg](const pb::Message& req, const std::shared_ptr<Writer>& w) {
            pb::Message resp{New(pool, pkg + ".ServerReflectionResponse")};
            resp.SetString("valid_host", req.GetString("host"));
            resp.Mutable("original_request") = req;
            const auto files{[&](const std::string& file) {
                pb::Message& fd{resp.Mutable("file_descriptor_response")};
                for (const std::string& f : pool.FileWithDependencies(file)) fd.AddString("file_descriptor_proto", *pool.FileProto(f));
            }};
            const auto error{[&](int code, const std::string& message) {
                pb::Message& e{resp.Mutable("error_response")};
                e.SetInt("error_code", code);
                e.SetString("error_message", message);
            }};
            const pb::FieldDesc* which{req.WhichOneof("message_request")};
            const std::string name{which ? which->name : ""};
            if (name == "list_services") {
                pb::Message& list{resp.Mutable("list_services_response")};
                for (const std::string& s : services) list.Add("service").SetString("name", s);
            } else if (name == "file_by_filename") {
                const std::string& f{req.GetString("file_by_filename")};
                if (pool.FileProto(f)) files(f);
                else error(5, "file not found: " + f);
            } else if (name == "file_containing_symbol") {
                std::string symbol{req.GetString("file_containing_symbol")};
                if (symbol.starts_with('.')) symbol.erase(0, 1);
                const std::string* f{pool.FileOfSymbol(symbol)};
                for (std::string s{symbol}; !f && s.find('.') != std::string::npos;) {
                    s.resize(s.rfind('.'));
                    f = pool.FileOfSymbol(s);
                }
                if (f) files(*f);
                else error(5, "symbol not found: " + symbol);
            } else if (name == "all_extension_numbers_of_type") {
                const std::string& type{req.GetString("all_extension_numbers_of_type")};
                if (pool.FindMessage(type)) resp.Mutable("all_extension_numbers_response").SetString("base_type_name", type);
                else error(5, "type not found: " + type);
            } else if (name == "file_containing_extension") {
                error(5, "extensions are not used");
            } else {
                error(3, "invalid reflection request");
            }
            w->Write(resp);
        });
    }
}

} // namespace grpcsrv
