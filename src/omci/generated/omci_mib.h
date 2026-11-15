/* The OMCI information model this device implements.
 * Generated from specification facts and the explicit compatibility profile.
 * Access, creation, notifications and option flags follow G.988 terminology.
 * Local strings store one trailing byte beyond their wire width.
 * Do not edit; regenerate with tools/mib-gen.py.
 */
#ifndef OMCI_MIB_H
#define OMCI_MIB_H

#include <stdint.h>

/* Numeric types use fixed widths; strings include a local terminator. */
enum omci_type { OMCI_U8, OMCI_U16, OMCI_U32, OMCI_U64, OMCI_STRING, OMCI_OCTETS };

struct omci_attr {
	const char *name;
	uint8_t type;       /* enum omci_type */
	uint16_t size;      /* stored bytes, including a string terminator */
	uint8_t oltAcc;     /* R=1, W=2, set-by-create=4 */
	uint8_t isIndex;
	uint8_t avc;        /* raises an attribute value change */
	uint16_t opt;       /* option mask: OMCI_OPT_* below */
};

/* Attribute requirement, support and upload flags.
 * The upload walk omits attributes selected by OMCI_OPT_NOT_UPLOADED. */
#define OMCI_OPT_MANDATORY      0x001
#define OMCI_OPT_M_NOT_SUPPORT  0x002
#define OMCI_OPT_OPTIONAL       0x004
#define OMCI_OPT_O_NOT_SUPPORT  0x008
#define OMCI_OPT_PRIVATE        0x010
#define OMCI_OPT_PM             0x100
#define OMCI_OPT_TABLE          0x200
#define OMCI_OPT_NOT_UPLOADED   0x31a

/* Class support and upload flags.
 * The upload walk omits classes selected by OMCI_STD_NOT_UPLOADED. */
#define OMCI_STD_PRIVATE        0x010
#define OMCI_STD_NO_UPLOAD      0x100
#define OMCI_STD_NOT_UPLOADED   0x110

struct omci_class {
	uint16_t classId;
	const char *name;
	uint16_t rowSize;   /* local row storage, at least the sum of attribute sizes */
	const struct omci_attr *attrs;
	uint8_t nattr;
	uint32_t actions;   /* 1 << msgType for every accepted action */
	uint16_t std;       /* class flags: OMCI_STD_* */
	uint8_t init;       /* 0: ONU-created; 1: OLT-created */
};

extern const struct omci_class omci_classes[];
extern const unsigned omci_class_count;

#endif
