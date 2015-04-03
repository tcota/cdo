#include <inttypes.h>
#include <limits.h>

#include "cdi.h"
#include "serialize.h"
#include "namespace.h"
#include "pio_util.h"

int
serializeGetSize(int count, int datatype, void *context)
{
  int (*serialize_get_size_p)(int count, int datatype, void *context)
    = (int (*)(int, int, void *))
    namespaceSwitchGet(NSSWITCH_SERIALIZE_GET_SIZE).func;
  return serialize_get_size_p(count, datatype, context);
}

void serializePack(void *data, int count, int datatype,
                    void *buf, int buf_size, int *position, void *context)
{
  void (*serialize_pack_p)(void *data, int count, int datatype,
                           void *buf, int buf_size, int *position, void *context)
    = (void (*)(void *, int, int, void *, int, int *, void *))
    namespaceSwitchGet(NSSWITCH_SERIALIZE_PACK).func;
  serialize_pack_p(data, count, datatype, buf, buf_size, position, context);
}

void serializeUnpack(void *buf, int buf_size, int *position,
                      void *data, int count, int datatype, void *context)
{
  void (*serialize_unpack_p)(void *buf, int buf_size, int *position,
                             void *data, int count, int datatype, void *context)
    = (void (*)(void *, int, int *, void *, int, int, void *))
    namespaceSwitchGet(NSSWITCH_SERIALIZE_UNPACK).func;
  serialize_unpack_p(buf, buf_size, position, data, count, datatype, context);
}



int
serializeGetSizeInCore(int count, int datatype, void *context)
{
  int elemSize;
  switch (datatype)
  {
  case DATATYPE_INT8:
    elemSize = sizeof (int8_t);
    break;
  case DATATYPE_INT16:
    elemSize = sizeof (int16_t);
    break;
  case DATATYPE_INT:
    elemSize = sizeof (int);
    break;
  case DATATYPE_FLT64:
    elemSize = sizeof (double);
    break;
  case DATATYPE_TXT:
  case DATATYPE_UCHAR:
    elemSize = 1;
    break;
  default:
    xabort("Unexpected datatype");
  }
  return count * elemSize;
}

void serializePackInCore(void *data, int count, int datatype,
                          void *buf, int buf_size, int *position, void *context)
{
  int size = serializeGetSize(count, datatype, context);
  int pos = *position;
  xassert(INT_MAX - pos >= size);
  memcpy((unsigned char *)buf + pos, data, (size_t)size);
  pos += size;
  *position = pos;
}

void serializeUnpackInCore(void *buf, int buf_size, int *position,
                            void *data, int count, int datatype, void *context)
{
  int size = serializeGetSize(count, datatype, context);
  int pos = *position;
  xassert(INT_MAX - pos >= size);
  memcpy(data, (unsigned char *)buf + pos, (size_t)size);
  pos += size;
  *position = pos;
}