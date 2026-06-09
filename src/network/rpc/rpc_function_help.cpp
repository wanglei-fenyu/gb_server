#include "rpc_function_help.h"
#include "network/io/message_meta.h"
#include <gbnet/buffer/compressed_stream.h>
#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message.h"
namespace gb
{
	void GetMsgData(gb::Meta& meta, ReadBufferPtr buffer, int meta_size, int64_t data_size, std::string& out_s)
	{
		std::string s = buffer->ToString();
		if ((CompressType)meta.compress_type == CompressType::CompressTypeNone)
		{
			out_s = s.substr(meta_size, data_size);
		}
		else
		{
			google::protobuf::io::ArrayInputStream         i(s.data() + meta_size, data_size);
			std::shared_ptr<AbstractCompressedInputStream> is(get_compressed_input_stream(&i, (CompressType)(CompressType)meta.compress_type));
			google::protobuf::io::CodedInputStream         c(is.get());
			uint32_t                                       size;
			c.ReadVarint32(&size);
			c.ReadString(&out_s, size);
		}
	}

}
