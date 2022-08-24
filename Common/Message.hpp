#pragma once
#include <cstdint>
#include <ostream>
#include <vector>
#include <fmt/format.h>

namespace SP {
	struct MessageHeader {
		uint32_t correlationId;
		uint32_t size = 0;
		long long creationTime;
		long long sendTime;
		long long returnTime;
	};

	struct Message {
		MessageHeader header;
		std::vector<uint8_t> body;
	};
} // namespace SP

template<> struct fmt::formatter<SP::Message> {
	constexpr auto parse(format_parse_context& ctx) -> decltype(ctx.begin()) {
		return ctx.end();
	}

	template<typename FormatContext> auto format(const SP::Message& input, FormatContext& ctx) -> decltype(ctx.out()) {
		return format_to(ctx.out(), "{{ \"header\": {{ \"correlationId\": {}, \"size\": {} }}, \"body\": \"{}\" }}",
						 input.header.correlationId, input.header.size,
						 std::string_view(reinterpret_cast<const char*>(input.body.data()), input.body.size()));
	}
};