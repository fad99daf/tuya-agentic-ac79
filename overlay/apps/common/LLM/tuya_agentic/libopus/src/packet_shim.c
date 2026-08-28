/* 本地补齐(2026-08-27):opus_packet_get_nb_frames 在上游定义于 src/opus_decoder.c,
 * 本移植只保留编码器、删了解码器,但 src/repacketizer.c 仍引用它(重打包 API,
 * 我们运行时不用,只是链接需要)。函数是纯 TOC 字节解析、无状态,照搬上游实现,
 * 不为此拖回整个解码器。 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "opus.h"

int opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len)
{
   int count;
   if (len<1)
      return OPUS_BAD_ARG;
   count = packet[0]&0x3;
   if (count==0)
      return 1;
   else if (count!=3)
      return 2;
   else if (len<2)
      return OPUS_INVALID_PACKET;
   else
      return packet[1]&0x3F;
}
