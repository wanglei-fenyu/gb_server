#include "rpc_function_help.h"
#include "network/io/message_meta.h"
#include <gbnet/buffer/compressed_stream.h>
//#include "google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "google/protobuf/message.h"
namespace gb
{
	void GetMsgData(gb::Meta& meta, ReadBufferPtr buffer, int meta_size, int64_t data_size, std::string& out_s)
	{
		std::string s = buffer->ToString(); // 鍘熷瀛楃涓叉嫹璐?

		if ((CompressType)meta.compress_type == CompressType::CompressTypeNone)
		{
			// 瀵逛簬鏈帇缂╃殑鏁版嵁锛岀洿鎺ユ埅鍙栭渶瑕佺殑閮ㄥ垎
			out_s = s.substr(meta_size, data_size);
		}
		else
		{
			// 瀵逛簬鍘嬬缉鐨勬暟鎹紝浣跨敤 protobuf 瑙ｇ爜
			google::protobuf::io::ArrayInputStream         i(s.data() + meta_size, data_size);
			std::shared_ptr<AbstractCompressedInputStream> is(get_compressed_input_stream(&i, (CompressType)(CompressType)meta.compress_type));
			google::protobuf::io::CodedInputStream         c(is.get());
			uint32_t                                       size;
			c.ReadVarint32(&size);

			// 瑙ｇ爜瀛楃涓插苟灏嗗叾瀛樺偍鍦ㄤ紶鍏ョ殑out_s涓?
			c.ReadString(&out_s, size);
		}
	}

}