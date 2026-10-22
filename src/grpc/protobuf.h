// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_GRPC_PROTOBUF_H
#define BITCOIN_GRPC_PROTOBUF_H

#include <univalue.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * A small protocol buffers implementation, driven by descriptors (a FileDescriptorSet, as protoc
 * writes it): messages are dynamic, their fields looked up by name. Enough for the interface of the
 * bip300301_enforcer (proto3, no extensions, no groups): the binary format, written canonically
 * (fields by number, packed repeated scalars, proto3 defaults left out, as the enforcer's encoder
 * writes them, so that a reply is byte for byte the enforcer's), and the JSON mapping of proto3,
 * which the Connect protocol carries.
 */
namespace pb {

enum class FieldType : uint8_t {
    DOUBLE = 1, FLOAT = 2, INT64 = 3, UINT64 = 4, INT32 = 5, FIXED64 = 6, FIXED32 = 7, BOOL = 8,
    STRING = 9, GROUP = 10, MESSAGE = 11, BYTES = 12, UINT32 = 13, ENUM = 14, SFIXED32 = 15,
    SFIXED64 = 16, SINT32 = 17, SINT64 = 18,
};

struct MessageDesc;

struct EnumDesc {
    std::string full_name;
    std::vector<std::pair<std::string, int32_t>> values;
    const std::string* Name(int32_t number) const;
    std::optional<int32_t> Number(std::string_view name) const;
};

struct FieldDesc {
    std::string name;
    std::string json_name;
    int32_t number{0};
    FieldType type{FieldType::INT32};
    bool repeated{false};
    //! Presence is tracked (a message, a member of a oneof, a proto3 `optional`): written when set,
    //! even with its default value. Otherwise a default value is not written.
    bool explicit_presence{false};
    //! Index of the oneof (a real one, not the synthetic one of a proto3 `optional`), or -1.
    int oneof_index{-1};
    std::string type_name;
    const MessageDesc* message{nullptr};
    const EnumDesc* enum_type{nullptr};

    bool IsPackable() const { return repeated && type != FieldType::STRING && type != FieldType::BYTES && type != FieldType::MESSAGE && type != FieldType::GROUP; }
    bool IsMap() const;
};

struct MessageDesc {
    std::string full_name;
    //! By field number.
    std::vector<FieldDesc> fields;
    std::vector<std::string> oneofs;
    bool map_entry{false};

    const FieldDesc* Field(int32_t number) const;
    /** By its name or its JSON name. */
    const FieldDesc* FieldByName(std::string_view name) const;
};

struct MethodDesc {
    std::string name;
    const MessageDesc* input{nullptr};
    const MessageDesc* output{nullptr};
    bool client_streaming{false};
    bool server_streaming{false};
    //! idempotency_level = NO_SIDE_EFFECTS: a read, which the Connect protocol may also serve to GET.
    bool no_side_effects{false};
};

struct ServiceDesc {
    std::string full_name;
    std::vector<MethodDesc> methods;
};

/** The descriptors of a FileDescriptorSet, with every type name resolved. */
class Pool
{
public:
    /** @return false, with `error` set, if the set cannot be read or names an unknown type. */
    bool Load(std::span<const uint8_t> file_descriptor_set, std::string& error);

    const MessageDesc* FindMessage(std::string_view full_name) const;
    const EnumDesc* FindEnum(std::string_view full_name) const;
    const ServiceDesc* FindService(std::string_view full_name) const;
    const std::vector<std::unique_ptr<ServiceDesc>>& Services() const { return m_services; }

    /** For server reflection: the serialized FileDescriptorProto of each file, by name. */
    const std::string* FileProto(std::string_view filename) const;
    /** The file that defines a symbol (a message, an enum, a service, or a method `Service.Method`). */
    const std::string* FileOfSymbol(std::string_view symbol) const;
    /** A file and those it imports, recursively, the file first. */
    std::vector<std::string> FileWithDependencies(const std::string& filename) const;

private:
    std::map<std::string, std::unique_ptr<MessageDesc>, std::less<>> m_messages;
    std::map<std::string, std::unique_ptr<EnumDesc>, std::less<>> m_enums;
    std::vector<std::unique_ptr<ServiceDesc>> m_services;
    std::map<std::string, std::string, std::less<>> m_files;
    std::map<std::string, std::vector<std::string>, std::less<>> m_file_deps;
    std::map<std::string, std::string, std::less<>> m_symbol_files;
};

/**
 * A message of some type. Fields are looked up by name (or JSON name); a name the type does not
 * have is a programming error (it throws std::logic_error).
 */
class Message
{
public:
    explicit Message(const MessageDesc& desc) : m_desc{&desc} {}
    Message(const Message& other);
    Message& operator=(const Message& other);
    Message(Message&&) noexcept = default;
    Message& operator=(Message&&) noexcept = default;
    ~Message();

    const MessageDesc& Desc() const { return *m_desc; }

    /** A singular field that was set (for a scalar without presence: set, or read from the wire). */
    bool Has(std::string_view name) const;
    size_t Count(std::string_view name) const;
    void Clear(std::string_view name);
    /** The member of a oneof that is set, by the oneof's name; nullptr if none. */
    const FieldDesc* WhichOneof(std::string_view oneof) const;

    // Singular scalars, read (the default if not set) and set.
    uint64_t GetUInt(std::string_view name) const;
    int64_t GetInt(std::string_view name) const;
    bool GetBool(std::string_view name) const;
    double GetDouble(std::string_view name) const;
    const std::string& GetString(std::string_view name) const;
    void SetUInt(std::string_view name, uint64_t value);
    void SetInt(std::string_view name, int64_t value);
    void SetBool(std::string_view name, bool value);
    void SetDouble(std::string_view name, double value);
    void SetString(std::string_view name, std::string value);

    // Singular messages: nullptr if not set (Sub: not GetMessage, a macro on Windows); Mutable sets it (empty) if it is not.
    const Message* Sub(std::string_view name) const;
    Message& Mutable(std::string_view name);

    // Repeated fields.
    const Message& At(std::string_view name, size_t index) const;
    Message& Add(std::string_view name);
    uint64_t UIntAt(std::string_view name, size_t index) const;
    const std::string& StringAt(std::string_view name, size_t index) const;
    void AddUInt(std::string_view name, uint64_t value);
    void AddString(std::string_view name, std::string value);

    /** The canonical binary form. */
    std::string Serialize() const;
    /** Merge the binary form `data` into this message. @return false, with `error` set, if it is malformed. */
    bool Parse(std::span<const uint8_t> data, std::string& error);

    /** How ToJson prints. */
    struct JsonOptions {
        //! The fields' own names (snake_case), not their JSON names (lowerCamelCase): the REST face.
        bool proto_names{false};
        //! Every field present: a field not set prints its default, one with presence (a message, a proto3
        //! `optional`) prints null, an empty repeated field []. ("EmitUnpopulated" of the protobuf libraries.)
        bool emit_unpopulated{false};
    };
    /** The proto3 JSON mapping. */
    UniValue ToJson() const { return ToJson(JsonOptions{}); }
    UniValue ToJson(const JsonOptions& options) const;
    bool FromJson(const UniValue& json, std::string& error);

    friend bool operator==(const Message& a, const Message& b);

    // Internal representation, by field number: each value is a scalar (u, d) or string (s) or message.
    struct Value {
        uint64_t u{0};
        double d{0};
        std::string s;
        std::unique_ptr<Message> m;
        Value() = default;
        Value(const Value& other);
        Value& operator=(const Value& other);
        Value(Value&&) noexcept = default;
        Value& operator=(Value&&) noexcept = default;
        friend bool operator==(const Value& a, const Value& b);
    };

private:
    const FieldDesc& Lookup(std::string_view name) const;
    std::vector<Value>& Slot(const FieldDesc& field);
    const Value* Singular(const FieldDesc& field) const;
    bool ParseImpl(std::span<const uint8_t> data, int depth, std::string& error);
    bool FromJsonImpl(const UniValue& json, int depth, std::string& error);
    void SerializeTo(std::string& out) const;

    const MessageDesc* m_desc;
    std::map<int32_t, std::vector<Value>> m_fields;
};

/** Varint and base64 helpers shared with the transport. */
void AppendVarint(std::string& out, uint64_t value);
std::string Base64Encode(std::string_view data);
std::optional<std::string> Base64Decode(std::string_view text);

} // namespace pb

#endif // BITCOIN_GRPC_PROTOBUF_H
