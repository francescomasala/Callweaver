
#include "fac.h"
#include "asn1.h"

#if 0
+-------------------------------
| IE_IDENTIFIER
+-------------------------------
| {length}
+-------------------------------
|   +---------------------------
|   | SERVICE_DISCRIMINATOR
|   +---------------------------
|   | COMPONENT_TYPE_TAG
|   +---------------------------
|   | {length}
|   +---------------------------
|   |	+-----------------------
|   |   | INVOKE_IDENTIFIER_TAG (0x2)
|   |   +-----------------------
|   |   | {length}              (0x1)
|   |   +-----------------------
|   |   | {value}               (odd integer 0-127)
|   |   +-----------------------
|   |   +-----------------------
|   |   | OPERATION_VALUE_TAG   (0x2)
|   |   +-----------------------
|   |   | {length}              (0x1)
|   |   +-----------------------
|   |   | {value}
|   |   +-----------------------
|   |	+-----------------------
|   |	| ASN.1 data
+---+---+-----------------------
#endif

enum {
	SUPPLEMENTARY_SERVICE 	= 0x91,
} SERVICE_DISCRIMINATOR;

enum {
	INVOKE 					= 0xa1,
	RETURN_RESULT 			= 0xa2,
	RETURN_ERROR 			= 0xa3,
	REJECT 					= 0xa4,
} COMPONENT_TYPE_TAG;

enum {
	INVOKE_IDENTIFIER 		= 0x02,
	LINKED_IDENTIFIER 		= 0x80,
	NULL_IDENTIFIER 		= 0x05,
} INVOKE_IDENTIFIER_TAG;

enum {
	OPERATION_VALUE 		= 0x02,
} OPERATION_VALUE_TAG;

enum {
	VALUE_QUERY 			= 0x8c,
	SET_VALUE 				= 0x8d,
	REQUEST_FEATURE 		= 0x8f,
	ABORT 					= 0xbe,
	REDIRECT_CALL 			= 0xce,
	CALLING_PARTY_TO_HOLD 	= 0xcf,
	CALLING_PARTY_FROM_HOLD = 0x50,
	DROP_TARGET_PARTY 		= 0xd1,
	USER_DATA_TRANSFER 		= 0xd3,
	APP_SPECIFIC_STATUS 	= 0xd2,

	/* not from document */
	CALL_DEFLECT 			= 0x0d,
} OPERATION_CODE;

enum {
	Q931_IE_TAG 			= 0x40,
} ARGUMENT_TAG;

#ifdef FACILITY_DEBUG
#define FAC_DUMP(fac,len,bc) fac_dump(fac,len,bc)
static void fac_dump (unsigned char *facility, unsigned int fac_len, struct misdn_bchannel *bc)
{
	int i;
	cb_log(0, bc->port, "    --- facility dump start\n");
	for (i = 0; i < fac_len; ++i)
		if ((facility[i] >= 'a' && facility[i] <= 'z') || (facility[i] >= 'A' && facility[i] <= 'Z') ||
			(facility[i] >= '0' && facility[i] <= '9'))
			cb_log(0, bc->port, "    --- %d: %04p (char:%c)\n", i, facility[i], facility[i]);
		else
			cb_log(0, bc->port, "    --- %d: %04p\n", i, facility[i]);
	cb_log(0, bc->port, "    --- facility dump end\n");
}
#else
#define FAC_DUMP(fac,len,bc)
#endif

static int enc_fac_calldeflect (__u8 *dest, char *number, int pres)
{
	__u8 *body_len,
		 *p = dest,
		 *seq1, *seq2;

	*p++ = SUPPLEMENTARY_SERVICE;
	*p++ = INVOKE;

	body_len = p++;

	p += _enc_int(p, 0x1 /* some odd integer in (0..127) */, INVOKE_IDENTIFIER);
	p += _enc_int(p, CALL_DEFLECT, OPERATION_VALUE);
	p += enc_sequence_start(p, &seq1);
	  p += enc_sequence_start(p, &seq2);
	    p += _enc_num_string(p, number, strlen(number), ASN1_TAG_CONTEXT_SPECIFIC);
	  p += enc_sequence_end(p, seq2);
	  p += enc_bool(p, pres);
    p += enc_sequence_end(p, seq1);
	
	*body_len = p - &body_len[1];
	
	return p - dest;
}

static void enc_ie_facility (unsigned char **ntmode, msg_t *msg, unsigned char *facility, int facility_len, struct misdn_bchannel *bc)
{
	__u8 *ie_fac;
	
	Q931_info_t *qi;

	ie_fac = msg_put(msg, facility_len + 2);
	if (bc->nt) {
		*ntmode = ie_fac + 1;
	} else {
		qi = (Q931_info_t *)(msg->data + mISDN_HEADER_LEN);
		qi->QI_ELEMENT(facility) = ie_fac - (unsigned char *)qi - sizeof(Q931_info_t);
	}

	ie_fac[0] = IE_FACILITY;
	ie_fac[1] = facility_len;
	memcpy(ie_fac + 2, facility, facility_len);

	FAC_DUMP(ie_fac, facility_len + 2, bc);
}

void fac_enc (unsigned char **ntmsg, msg_t * msg, enum facility_type type,  union facility fac, struct misdn_bchannel *bc)
{
	__u8 facility[256];
	int len;

	switch (type) {
	case FACILITY_CALLDEFLECT:
		len = enc_fac_calldeflect(facility, fac.calldeflect_nr, 1);
		enc_ie_facility(ntmsg, msg, facility, len, bc);
		break;
	case FACILITY_CENTREX:
	case FACILITY_NONE:
		break;
	}
}

void fac_dec (unsigned char *p, Q931_info_t *qi, enum facility_type *type,  union facility *fac, struct misdn_bchannel *bc)
{}
