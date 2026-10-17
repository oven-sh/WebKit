/*
 * Minimal Mach type definitions for compiling Apple's `migcom` (the Mach
 * Interface Generator backend, from apple-oss-distributions/bootstrap_cmds)
 * as a *Linux host tool*, so WebKit's MachExceptions.defs can be processed
 * when cross-compiling JavaScriptCore for macOS on a Linux builder.
 *
 * Everything here is copied from the macOS SDK's <mach/message.h> /
 * <mach/port.h>. Only the subset migcom's own .c files reference is
 * defined. The struct layouts MUST match the SDK exactly (including the
 * `#pragma pack(push, 4)` and the LP64 field ordering) — migcom uses
 * sizeof() on the descriptor types when computing the message-size
 * arithmetic it bakes into the generated mach_excUser.c/mach_excServer.c.
 * The build host is always LP64, matching the LP64 macOS targets.
 *
 * Validated by diffing `migcom`'s output for <mach/mach_exc.defs> against
 * the mig output Apple ships in llvm-project's
 * lldb/tools/debugserver/source/MacOSX/mach_excServer.c.
 */
#ifndef _MIG_LINUX_MACH_COMPAT_H
#define _MIG_LINUX_MACH_COMPAT_H

#include <stdint.h>

typedef int boolean_t;
typedef int kern_return_t;
typedef int integer_t;
typedef unsigned int natural_t;
typedef natural_t mach_msg_type_number_t;
typedef unsigned int mach_msg_bits_t;
typedef natural_t mach_msg_size_t;
typedef integer_t mach_msg_id_t;
typedef unsigned int mach_port_name_t;
typedef mach_port_name_t mach_port_t;
typedef unsigned int mach_msg_copy_options_t;
typedef unsigned int mach_msg_type_name_t;
typedef unsigned int mach_msg_descriptor_type_t;
typedef unsigned int mach_msg_guard_flags_t;
typedef uint64_t mach_port_context_t;

typedef struct {
  mach_msg_bits_t msgh_bits;
  mach_msg_size_t msgh_size;
  mach_port_t msgh_remote_port;
  mach_port_t msgh_local_port;
  mach_port_name_t msgh_voucher_port;
  mach_msg_id_t msgh_id;
} mach_msg_header_t;

typedef struct {
  unsigned char mig_vers;
  unsigned char if_vers;
  unsigned char reserved1;
  unsigned char mig_encoding;
  unsigned char int_rep;
  unsigned char char_rep;
  unsigned char float_rep;
  unsigned char reserved2;
} NDR_record_t;

typedef struct {
  kern_return_t RetCode;
} mig_reply_error_t_body; /* unused; placeholder */

#pragma pack(push, 4)

typedef struct {
  natural_t pad1;
  mach_msg_size_t pad2;
  unsigned int pad3 : 24;
  mach_msg_descriptor_type_t type : 8;
} mach_msg_type_descriptor_t;

typedef struct {
  mach_port_t name;
  mach_msg_size_t pad1;
  unsigned int pad2 : 16;
  mach_msg_type_name_t disposition : 8;
  mach_msg_descriptor_type_t type : 8;
} mach_msg_port_descriptor_t;

/* LP64 layout (the build host and every macOS target are LP64). */
typedef struct {
  void *address;
  boolean_t deallocate : 8;
  mach_msg_copy_options_t copy : 8;
  unsigned int pad1 : 8;
  mach_msg_descriptor_type_t type : 8;
  mach_msg_size_t size;
} mach_msg_ool_descriptor_t;

typedef struct {
  void *address;
  boolean_t deallocate : 8;
  mach_msg_copy_options_t copy : 8;
  mach_msg_type_name_t disposition : 8;
  mach_msg_descriptor_type_t type : 8;
  mach_msg_size_t count;
} mach_msg_ool_ports_descriptor_t;

typedef struct {
  mach_port_context_t context;
  mach_msg_guard_flags_t flags : 16;
  mach_msg_type_name_t disposition : 8;
  mach_msg_descriptor_type_t type : 8;
  mach_port_name_t name;
} mach_msg_guarded_port_descriptor_t;

typedef struct {
  mach_msg_size_t msgh_descriptor_count;
} mach_msg_body_t;

typedef struct {
  mach_msg_header_t header;
  mach_msg_body_t body;
} mach_msg_base_t;

#pragma pack(pop)

#define MACH_MSG_PORT_DESCRIPTOR 0
#define MACH_MSG_OOL_DESCRIPTOR 1
#define MACH_MSG_OOL_PORTS_DESCRIPTOR 2
#define MACH_MSG_OOL_VOLATILE_DESCRIPTOR 3
#define MACH_MSG_GUARDED_PORT_DESCRIPTOR 4

#define MACH_MSG_TYPE_MOVE_RECEIVE 16
#define MACH_MSG_TYPE_MOVE_SEND 17
#define MACH_MSG_TYPE_MOVE_SEND_ONCE 18
#define MACH_MSG_TYPE_COPY_SEND 19
#define MACH_MSG_TYPE_MAKE_SEND 20
#define MACH_MSG_TYPE_MAKE_SEND_ONCE 21
#define MACH_MSG_TYPE_COPY_RECEIVE 22
#define MACH_MSG_TYPE_PORT_NONE 0
#define MACH_MSG_TYPE_PORT_NAME 15
#define MACH_MSG_TYPE_PORT_RECEIVE MACH_MSG_TYPE_MOVE_RECEIVE
#define MACH_MSG_TYPE_PORT_SEND MACH_MSG_TYPE_MOVE_SEND
#define MACH_MSG_TYPE_PORT_SEND_ONCE MACH_MSG_TYPE_MOVE_SEND_ONCE
#define MACH_MSG_TYPE_POLYMORPHIC ((mach_msg_type_name_t)-1)
#define MACH_MSG_TYPE_PORT_ANY(x) (((x) >= MACH_MSG_TYPE_MOVE_RECEIVE) && ((x) <= MACH_MSG_TYPE_MAKE_SEND_ONCE))

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif /* _MIG_LINUX_MACH_COMPAT_H */
