
#include "asn1.h"
#include <string.h>

int _enc_null (__u8 *dest, int tag)
{
	dest[0] = tag;
	dest[1] = 0;
	return 2;
}

int _enc_bool (__u8 *dest, __u32 i, int tag)
{
	dest[0] = tag;
	dest[1] = 1;
	dest[2] = i ? 1:0;
	return 3;
}

int _enc_int (__u8 *dest, __u32 i, int tag)
{
	__u8 *p;
	dest[0] = tag;
	p = &dest[2];
	do {
		*p++ = i;
		i >>= 8;
	} while (i);
	dest[1] = p - &dest[2];
	return p - dest;
}

int _enc_enum (__u8 *dest, __u32 i, int tag)
{
	__u8 *p;

	dest[0] = tag;
	p = &dest[2];
	do {
		*p++ = i;
		i >>= 8;
	} while (i);
	dest[1] = p - &dest[2];
	return p - dest;
}

int _enc_num_string (__u8 *dest, __u8 *nd, __u8 len, int tag)
{
	__u8 *p;
	int i;

	dest[0] = tag;
	p = &dest[2];
	for (i = 0; i < len; i++)
		*p++ = *nd++;
	dest[1] = p - &dest[2];
	return p - dest;
}

int _enc_sequence_start (__u8 *dest, __u8 **id, int tag)
{
	dest[0] = tag;
	*id = &dest[1];
	return 2;
}

int _enc_sequence_end (__u8 *dest, __u8 *id, int tag_dummy)
{
	*id = dest - id - 1;
	return 0;
}

