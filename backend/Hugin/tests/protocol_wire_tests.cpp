#include "test_common.hpp"

#include <type_traits>

#include "../protocol/wire.hpp"

bool run_protocol_wire_tests() {
    using namespace hugin::protocol;

    ClientHello hello;
    hello.host_id = "host-a";
    hello.client_instance_id = "instance-1";
    hello.requested_protocol_version = 1;
    hello.resume_from_sequence = 10;
    hello.capabilities_bitmask = 3;
    hello.auth_token = "token";

    const auto encoded = serialize_message(Message {hello});
    const auto decoded = parse_message(encoded);
    require_true(decoded.ok(), "ClientHello should decode");
    require_true(std::holds_alternative<ClientHello>(decoded.value), "Decoded message should be ClientHello");
    const auto decoded_hello = std::get<ClientHello>(decoded.value);
    require_true(decoded_hello.host_id == hello.host_id, "ClientHello host_id mismatch");
    require_true(decoded_hello.client_instance_id == hello.client_instance_id, "ClientHello instance mismatch");
    require_true(decoded_hello.resume_from_sequence == hello.resume_from_sequence, "ClientHello resume mismatch");

    DataMessage data;
    data.host_id = "host-b";
    data.sequence = 99;
    data.codec_version_major = 1;
    data.codec_version_minor = 0;
    data.sent_at_unix_seconds = 1'700'000'123;
    data.frame_crc32c = 0;
    data.muninn_frame = {1U, 2U, 3U, 4U};

    const auto encoded_data = serialize_message(Message {data});
    const auto decoded_data = parse_message(encoded_data);
    require_true(decoded_data.ok(), "Data message should decode");
    require_true(std::holds_alternative<DataMessage>(decoded_data.value), "Decoded message should be Data");
    const auto parsed_data = std::get<DataMessage>(decoded_data.value);
    require_true(parsed_data.host_id == data.host_id, "Data host_id mismatch");
    require_true(parsed_data.sequence == data.sequence, "Data sequence mismatch");
    require_true(parsed_data.muninn_frame == data.muninn_frame, "Data payload mismatch");

    return true;
}
