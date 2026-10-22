# BT NAP PoC

## Current phase

The executable connects the firmware ENIP IPv4 stack directly to Bluetooth
PAN. It creates private process group `0x1a`, GBS process `0x1a00` and a BTA
accessor, opens an L2CAP connection to PSM `0x000f` and completes BNEP setup.
After setup, `bt_pan_netif` registers an ENIP driver and becomes the `bt_panu`
receive callback.

Incoming BNEP IPv4 payloads go directly to ENIP. ENIP egress is wrapped in an
Ethernet/BNEP frame and sent through `bt_panu`. ARP remains a separate protocol
module called by `bt_pan_netif`; BNEP filter requests remain in `bt_panu`.
IPv4 multicast is dropped before ENIP, while IPv6 is not connected to the
IPv4-only driver. Unsupported BNEP filter requests receive the corresponding
negative response instead of being silently ignored.

Do not include dietlibc's `netinet/ip.h` after `swilib.h`: it redeclares
`ntohl` and bypasses the SWILIB SWI binding. The first IGMP packet then called
an invalid RAM address. Multicast detection reads the first destination-address
octet directly from the network-order IPv4 header instead.

Incoming IPv4 payloads are copied into `ENIP_PKT` objects and sent to the
driver's `port_rx`. Native ENIP egress packets are drained from the link
module's port queue, wrapped in Ethernet/BNEP, sent to Bluetooth and released.

The phone currently connects as PANU to a NAP running on the host. Stock
EL71sw45 limits both L2CAP configuration fields and RX reassembly to 335
bytes. The current `bt-pan-mtu.vkp` raises the internal limit to 1696 bytes,
enlarges all 26 generic RX ACL objects from `0x1b4` to `0x704`, and enlarges
all 36 generic TX ACL objects from `0x174` to `0x704`; the ELF requests the
BNEP minimum L2CAP MTU of 1691 bytes and exposes an IPv4 MTU of 1500 through
ENIP. This is not yet the final phone-as-NAP mode.

The ACL buffers are obtained through `MemOAlloc`, but the stock generic ACL
heaps are fixed-size Nucleus partition pools rather than variable-size heaps.
The current experimental patch keeps the generic RX object count at 26 but
enlarges every object to `0x704`. Fragmented and unfragmented packets therefore
use the original allocator, owner, counters and release path. The allocator's
maximum request and the reassembly allocation are raised from `0x155` to
`0x6a0`; its fixed-block `MemOAlloc` request is raised from `0x1ac` to `0x700`.
The latter fits in each `0x704` partition object.

TX has a separate contiguous-buffer requirement. `BTLP_uiSendDataOverBT`
copies up to the negotiated L2CAP MTU into one object returned by
`FUN_A0920028`; that function still requests a nominal `0x155` bytes from the
fixed `BT_TX_ACL_HEAP`. With MTU 1691, a 1515-byte BNEP frame therefore
overflowed the stock `0x174` object and corrupted the Nucleus partition free
list. The crash appeared on the next allocation at `NU_Allocate_Partition +
0x94` (`0xA00A04EC`). The patch preserves all 36 TX objects but enlarges each
to `0x704`, adding `0xc840` (51264 bytes) of backing. Lower layers retain the
original HCI fragmentation and free paths.

The boot heap table stores each size before the following heap name. Generic
`BT_RX_ACL_HEAP` is therefore `0x2c48`, not `0x6c4`. `0x6c4` belongs to
`BT_BNEPTX_ACL_HEAP`, `0x704` belongs to `BT_BNEPRX_ACL_HEAP`, and `0x2d80`
belongs to `BT_PRIM_HEAP`. This explains the earlier boot failures: the
supposed RX/BNEP swap actually modified BNEP TX and PRIM. The boot heap table
must not be swapped with adjacent descriptors. The current experiment changes
only the generic RX descriptor's object size and lets `MemOIni` calculate and
allocate the larger backing.

Both ACL heaps are fixed-block pools. Increasing only the allocation request
still triggers `MemBPAllocSizeInval`, because the stock RX object is `0x1b4`
and the stock TX object is `0x174`. The rejected experiment changed RX to six
`0x700` objects and TX to seven `0x6c0` objects. The phone booted, but OBEX
exhausted RX ACL and crashed in `L2CAP_CC_PROCESS`; the generic pool geometry
must remain stock.

The BNEP heap names do not imply that EL71 contains a complete native BNEP
implementation. In both EL71 and `emulator.exe`, their descriptors are only
referenced by the memory initialization image and the global heap registry.
There are no executable references to either BNEP heap handle, no BNEP
allocator/free/data handler, and no NAP/GN SDP UUIDs (`0x1116`/`0x1117`). The
platform configuration reserved one BNEP-sized TX and RX object, but the
module that used them was excluded. A custom PAN implementation can still use
these pools selectively for large L2CAP SDUs.

A runtime probe confirmed both dormant pools on the phone. `BT_BNEPTX_ACL_HEAP`
allocated and freed its single 1732-byte object; `BT_BNEPRX_ACL_HEAP` did the
same with its single 1796-byte object. The temporary probe was then removed
from startup. Normal PAN traffic should acquire the RX object only while
assembling a large L2CAP SDU; TX continues to use stock ACL fragments.

## MTU allocation alternatives

`MemOAlloc` dispatches by `MemObject.flags`: type 0 is a variable-size Nucleus
memory pool and type 1 is a fixed-size partition pool. The type is established
by `MemOIni`; changing the flag after initialization would reinterpret an
incompatible control block and corrupt memory.

The initial design was a selective large-RX path through the existing
one-object `BT_BNEPRX_ACL_HEAP`. L2CAP currently has one global segmented-SDU
reassembly context, so a second fixed object would not add parallel
reassembly. TX already fragments a complete L2CAP SDU into stock ACL buffers.
Consequently the generic ACL pools and their buffer-count flow control do not
need any geometry changes.

The current phone experiment instead uses the simpler global fixed-pool
variant: all 26 generic RX objects and all 36 generic TX objects are `0x704`.
It keeps both stock counts and their flow-control semantics, costs 86624 bytes
(about 84.6 KiB) of additional backing, and leaves the stock allocation and
release paths unchanged.

There is one practical fallback. `ENIP_HEAP` at descriptor `0xa8f58658`,
handle `0xa8f586a0`, is an initialized variable-size `0x2000`-byte pool with
no executable references found in EL71sw45. A selective PAN RX allocator could
use it for roughly four `0x700` allocations. It must be runtime-probed before
use; the active `ENIP1_HEAP` and `ENIP2_HEAP` are different pools and must not
be substituted.

An exhaustive scan of all 44 firmware `MemObject` entries found one more
unreferenced large variable pool: `VOICE_HEAP`, descriptor `0xa8f58ce0`,
handle `0xa8f58d28`, capacity `0x3fc4`. It could hold roughly nine large RX
allocations, but it is only a conditional fallback: indirect registry access
is possible, and coexistence with normal and Bluetooth voice calls must be
tested. Every other sufficiently large pool has direct consumers (`OBEX`,
`LLC`, `SNDCP`, `ENIP1/2`, `SIM`, display and others). The complete inventory
is in `../../sgold/EL71-bluetooth-pan.md`.

Converting `BT_BNEPRX_ACL_HEAP` itself to variable-size before `MemOIni` is
possible, but its resulting pool would still hold only one `0x700` allocation,
so it gains nothing over the fixed object. Converting the generic RX pool is
also technically possible only before `MemOIni`, but its free/reserved
counters count fixed objects rather than bytes. Supporting mixed sizes would
require redesigning Bluetooth flow control and would affect Java and OBEX.

A global fixed-pool variant can preserve all 26 RX partitions while enlarging
each from `0x1b4` to `0x704`. It avoids mixed-size counter semantics, but costs
about `0x8a20` (35 KiB) of additional permanent backing and wastes a large
partition for every short ACL packet. The earlier failed experiment kept the
backing small and reduced the count to six; preserving 26 would likely avoid
that specific OBEX exhaustion, but is still substantially worse than the
selective BNEP RX object.

Other choices are inferior: global `malloc` competes with the whole system;
fragment chains require changing the contiguous L2CAP/SAPI/JSApi ABI; a custom
L2CAP must reimplement signaling, CIDs, security, flow control and reassembly;
IPv4 fragmentation with a route MTU around 320 avoids the patch but does not
provide a real BNEP MTU of 1500. The complete comparison is recorded in
`../../sgold/EL71-bluetooth-pan.md`.

The first full-MTU phone test should apply only the current 26-object generic
RX patch.
After negotiation reports 1691 in both directions, run:

```sh
ping -I btnap0 -M do -c 3 -s 1472 192.168.7.2
```

This produces an exact 1500-byte IPv4 packet and exercises large RX and TX in
one echo exchange. Follow it with a 100-packet burst, repeat ELF start/stop,
and transfer data over OBEX concurrently. If OBEX or another Bluetooth profile
fails, revert this global experiment and return to a selective allocator
backed by `BT_BNEPRX_ACL_HEAP` or the separately probed `ENIP_HEAP`.

### Linux PB=0 fragmentation compatibility

Linux uses HCI PB=0 (`ACL_START_NO_FLUSH`) for the first fragment of a
non-flushable L2CAP packet. It also uses PB=0 when the whole packet fits in one
ACL packet. The EL71 receive path maps PB=0 directly to its complete-packet
state, so a packet split by the host controller as PB=0 followed by PB=1 is
not reassembled correctly.

The relevant code is `FUN_A092279A` from
`implementation/btoothf/btlsar.c`, before the L2CAP receive handler. It
extracts PB from bits 12..13 of the HCI handle/flags word, accepts PB=2 as a
first fragment and PB=1 as a continuation, but frees every other value. Thus
Linux PB=0 never reaches the later L2CAP code at all.

The hook at `0xA09227F2` in `bt-pan-mtu.vkp` normalizes PB=0 to PB=2 before
the firmware's original dispatch. The same BTLSAR function has two aggregate
guards with an inclusive `0x15a` maximum: one before appending a continuation
and one before accepting the completed aggregate. The patch raises both to
`0x6a0` at `0xA0922884` and `0xA09228B2`. This works with any peer ACL MTU;
no `hci0` MTU override is required.

The JSR-82/JSApi bridge has a second buffer above L2CAP. Each registered
connection originally allocates a `0x400`-byte ring, with the same size
hard-coded in its read/write modulo arithmetic. The VKP changes the allocation,
capacity check and both modulo operations to `0x800`. This holds a complete
1691-byte packet plus the private two-byte packet-length prefix.

The ring cannot remain `0x400` and deliver one packet in pieces. The SApp RX
callback has producer pause/resume flags, but `JSApi_ReadBytes` consumes the
packet-length prefix before draining the available payload. A refill would no
longer have a valid prefix, so packet framing is lost. `0x800` is the smallest
convenient power-of-two capacity above 1693 bytes. `0x1000` could queue two
full SDUs but is not required for correctness. A per-connection capacity would
require adding a field to `JSApi_Handle_t` and replacing the helpers' global
hard-coded modulo.

`JSApi_BytesAvailable` reports bytes, not complete packets. Under continuous
MTU-sized traffic the `0x800` ring can contain one complete SDU followed by a
second SDU's two-byte length and partial payload. Calling `JSApi_ReadBytes`
at that point consumes the length even when the payload is incomplete and
breaks all following packet boundaries. The poller now peeks the little-endian
length at the ring's current read offset and calls `JSApi_ReadBytes` only when
`available >= length + 2`. Reading a complete packet invokes the firmware's
resume callback, allowing the paused producer to fill the newly freed space.

The producer itself is not atomic: `JSApi_SAppReceive @ 0xA0A0ED24` originally
writes the two-byte length first and then copies as much payload as fits. A
short payload write leaves an incomplete record and sets `paused`. The full
VKP now hooks this callback before the prefix write and checks that the ring
has space for `length + 2`. If it does not, nothing is written and the stock
short-write path pauses the producer. Reading an earlier complete packet then
invokes the stock resume callback, which retries the entire SDU with its
packet boundary intact.

The JSApi callback drains BTLP's private receive queue, but it does not rearm
L2CAP after BTLP has returned flow-control. The channel then stays at RX state
zero and new SDUs accumulate in `l2cap_rx_queue_heads`. Primitive `0x00e3` is
named `L2CA_ReadDataReq` by the firmware. Its handler at `0xA08C8848` changes
the channel RX state back to one and delivers queued SDUs until the upper
layer applies flow-control again. Every RX poll invokes it whenever the
channel RX state is zero, including when JSApi has no complete packet ready.
The active CID is read from the BTLP connection table. SApp
`SDeviceInstanz` contains a BTLP SDevice in the `0x0d..0x15` range. The stock
BTLP send and receive paths convert it to a table index by subtracting
`0x0d`. The byte at offset seven of the selected entry is the separate LDA
application instance. PAN therefore does not assume that it owns CID
`0x0040`.

The ELF also owns `bt_panu.rx_buf[0x700]`, after which the IPv4 payload is
copied into an `ENIP_PKT`. Avoiding these upper copies requires bypassing
JSApi with a private SApp receive callback or adding a new zero-copy API. It
does not remove the lower contiguous L2CAP reassembly allocation and is a
separate optimization from enabling MTU 1500.

The direct ENIP path is verified with ICMP payloads of 4 and 280 bytes. ENIP
RX packets must use the native link layout: allocate `payload_size + 9` bytes
and leave four bytes of headroom before `packet->data`. Allocating only the
payload corrupted the NETCORE heap during `enip_port_send`.

Full IPv4 MTU 1500 is verified after the BTLSAR PB=0 fix and the matching RX
and TX pool enlargement. A continuous host-side
`ping -I btnap0 -M do -s 1472 192.168.7.2` completed 12 echo exchanges with
zero loss. Each request carries a 1500-byte IPv4 packet and exercises Linux
ACL fragmentation, EL71 BTLSAR reassembly, ENIP ICMP processing, and the
1515-byte BNEP/BTLP transmit path on the reply.

The HCI Host Buffer Size command must remain stock (`acl=339`, builder bytes
`5322/0122`). An early experiment changed it to `acl=1696`
(`A022/0622`). That setting is unrelated to L2CAP MTU and can retain or exhaust
the controller's five host-buffer credits across reconnects: a few packets
arrive and RX then stops below L2CAP. Since removing an entry from a later VKP
does not undo bytes already flashed, `revert-hci-host-buffer.vkp` restores the
two instructions once. The full PAN patch keeps the large L2CAP/reassembly
buffers while allowing normal 339-byte HCI fragmentation.

Phone-originated ICMP is also verified. The standalone `icmp_probe` queues an
echo request for `192.168.7.1` through the ENIP link port. The host resolves
`192.168.7.2` with ARP, receives the phone's ARP reply and returns the ICMP
echo reply. ENIP allocations include an internal eight-byte prefix, so a TX
packet containing `N` bytes must request at least `N + 8` bytes while keeping
`packet->size` equal to `N`.

The fourth argument of `enip_packet_allocate` is a data offset, not an owner
ID. The firmware rounds offset 7 up to the next four-byte boundary and sets
`packet->data = packet->reserved_10 + 8`. The requested allocation size
includes this headroom.

Set the host adapter address, in printed byte order, in `config.h`:

```c
#define BT_PANU_HOST_BDADDR { 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff }
```

An all-zero address is rejected. USART reports the accessor and connection
handles, the mapped BTA events, BNEP setup and ENIP registration result.

## Recovered firmware path

- `BTA_Accessor_New(cepid, application_message_id, 0)` routes events to a GBS
  process. Accessor events are long GBS messages: the logical ID is
  `msg & 0xffff`. `GBS_GetMessagePayload(message)` returns a payload whose
  first word is the BTA object-message pointer and whose next halfword is the
  accessor handle. Reading `submess/data0` directly only works for short GBS
  messages and produced zeroes in the first diagnostic build.
- `BTA_Accessor_MapObjectMessage` at `0xA07DD9CA` maps the internal object
  class/message to the application event ID. The upper 16 bits contain the
  object handle.
- Event `0` is `ACCESSOR_STARTED_CFM`. Requests using a newly created accessor
  must wait for its successful completion; issuing L2CAP open immediately
  after `BTA_Accessor_New` produced open failure `0x100c`.
- EL71's LDA configuration validator at `0xA0920714` rejects either L2CAP MTU
  field above `0x014f` with error `0x100c`. It is called by `LDAEstReq` at
  `0xA0920CAE`, `LDAReconfReq` at `0xA0920DAA`, and another LDA configuration
  handler at `0xA09210A8`. `bt-pan-mtu.vkp` changes the validator limit to
  1696, while the ELF requests 1691.
- Segmented RX checks the complete L2CAP SDU length at `0xA0922256`. Its
  comparison uses `r1`, unlike the LDA validator, which uses `r2`. The patch
  must therefore encode `movs r1, #0xd4; lsls r1, r1, #3` (`D421 C900`) to
  produce 1696. Reusing the validator's `r2` sequence negotiates 1691 but does
  not correctly replace this RX check.
- Before L2CAP, the outer incoming ACL primitive parser at `0xA0922E04`
  rejects containers above `0x014F`. Its comparison value at `0xA0922E0C`
  and matching error-report value at `0xA0922E38` must be raised first.
  Primitive type handlers at `0xA0923042` and `0xA09230C4` then independently
  reject payloads above `0x0155`. Their limits are formed at `0xA0923058` in
  `r6` and at `0xA09230DA` in `r7`; both values are also passed to the generic
  RX allocator. Negotiating 1691 and enlarging the reassembly buffer is
  therefore insufficient by itself: `bt-pan-mtu.vkp` raises all pre-L2CAP
  limits and allocation requests to `0x06A0`.
- The BTOS adapter vtable exposes `GetDataSizeRxAcl` at `0xA08BB3AE`. The
  stock callback returns `0x0158`, so the lower transport still treats an RX
  ACL buffer as a roughly 341-byte payload even after the pool is enlarged.
  The patch returns `0x06A8`: enough for the `0x06A0` payload plus transport
  overhead, while remaining below the `0x0700` fixed-block allocation.
- `HCI_BuildHostBufferSizeCommand` at `0xA08C59E4` builds standard HCI
  command `0x0C33`. Stock firmware writes `0x0153` (339) as Host ACL Data
  Packet Length and advertises five host ACL buffers. This is a controller
  flow-control contract, separate from the later host-side parser limits.
  Testing `0x06A0` confirmed that the command was accepted and that the ARM
  parser capacity was `0x06A8`, but a 600-byte ping still never reached that
  parser. Restoring `0x0153` also did not make the controller split the peer's
  large HCI ACL packet: neither the HCI parser nor the L2CAP reassembly state
  changed. Fragment boundaries must therefore be made smaller by the sending
  host. The main patch restores the two stock instructions on phones carrying
  the earlier experimental value.
- The HCI byte-stream parser reads that saved capacity before receiving the
  ACL payload and branches to an error path at `0xA052AB14` when `dlen + 5`
  exceeds it. A host `btmon` capture proved that BlueZ sends the 600-byte ping
  as one `dlen=641` ACL packet containing a 637-byte BNEP SDU. Forcing this
  branch to the accepted path did not make the packet reach L2CAP, proving
  that it is lost before this host-side parser; the diagnostic branch patch
  was removed.
- The stock JSR-82 L2CAP client calls `JSApi_InitHandles()` at `0xA0A0EB44`
  before `BTA_Conn_New_L2CAP`, then registers the returned native handle with
  `JSApi_RegisterConHandle(handle, 1, 0)` at `0xA0A0DE1C`. The initial PoC
  omitted this VIO/SAPI bridge and still received open failure `0x100c` after
  correctly waiting for event `0`. These three JSApi entry points are ARM, not
  Thumb; their function pointers must keep bit 0 clear.
- `JSApi_ClearConnectionHandle(handle)` at `0xA0A0E440` maps the native BTA
  handle back to one of five JSApi connection slots, frees its `0x400`-byte
  buffer and resets the slot. The PoC calls it on open failure, close, and its
  three-second timeout so repeated ELF launches do not exhaust the pool.
- Connection-open event `9` contains result at offset `0x18`, remote-device
  handle at `0x20`, and connection type at `0x22`.
- `BTA_Conn_GetSapiApp(accessor, connection)` returns the SAPI used by the
  data path. For JSR-82 L2CAP the observed value is `0x0f`.
- `BTLP_uiSendDataOverBT(data, length, sapi)` at `0xA08D10AC` is the actual
  `SApp_IO.SendData` entry and returns the number of queued bytes. It must not
  be called directly with the value returned by `BTA_Conn_GetSapiApp`: the
  third argument is the VIO `SDeviceInstanz`, not merely the SAPI number.
- The stock `btl2cap_Protocol_nativeSend` calls
  `JSApi_WriteBytes(connection_handle, data, length)` at `0xA0A0DB68`. This
  resolves the registered connection to its `SApp_IO`, checks that the VIO
  side is attached, and invokes `SendData` with the correct `SDeviceInstanz`.
  The PoC defers the first write outside the BTA event handler and briefly
  retries while the JSApi slot is still in pending state `2`; ready is state
  `3`.
- The stock `btl2cap_Protocol_nativeReceive` calls
  `JSApi_ReadBytes(connection_handle, buffer, capacity)` at `0xA0A0D7B0`.
  The matching x86 reference is `emulator.exe:0x0142BA40`. For packet
  connections the JSApi queue stores a private 16-bit SDU length before each
  packet; `JSApi_ReadBytes` consumes that prefix internally and returns only
  the L2CAP/BNEP payload. `JSApi_BytesAvailable(handle)` at `0xA0A0DC60`
  reports the number of queued bytes and is used by the PoC's short timer
  poller. Both functions are ARM entry points.
- `BT_GetLocalAddress()` at `0xA0643E5A` returns the six-byte firmware-order
  local address. Bluetooth firmware order is reversed relative to printed
  MAC/BNEP order.

These names and prototypes were also applied to the open EL71 Ghidra project.

The SDK's `CreateICL` SWI `0x38e` is unresolved on this ELKA firmware: calling
it exits at `0xffff38e0`. Ghidra identifies the direct Thumb implementation as
`CreateICL @ 0xa0092450`; the PoC calls `0xa0092451` through
`CreateICLDirect` instead of the SWI wrapper.

The diagnostic build prints every message received by CEPID `0x1a00` and
starts shutdown after the configured test interval. Shutdown follows the stock two-phase
`btl2cap_Protocol_nativeClose` sequence: first
`BTA_Conn_CloseReq(accessor, connection, transaction)`, then, after
`CONNECTION_CLOSED_IND/CFM`, `JSApi_ClearConnectionHandle(connection)`.
Only after the native connection is gone does it delete the BTA accessor, kill
the private GBS process, and call `kill_elf()`. The earlier build cleared the
JSApi slot and deleted the accessor while L2CAP was still live, orphaning
Bluetooth state until a phone reboot. If a close event is delayed, the new
build deliberately remains alive and continues servicing BTA messages instead
of recreating that orphaned state. On this firmware the close request may
complete through the JSR-82 transaction path without an accessor close event.
The watchdog therefore implements the native close function's second pass:
once `JSApi_GetConnectionState` reports closed state `0` or `2`, it clears
the JSApi handle and performs final cleanup.

Closing the L2CAP channel does not close its base ACL link. BlueZ can then
retain `network_adapter.setup` and reject the next PSM `0x000f` connection
with `Refusing connect ...: setup in progress`; the rejected attempt clears
the stale setup, producing an apparent every-other-launch failure. A manual
host-side ACL disconnect proved this cause. The PoC now submits the firmware's
original internal `BTA_vBtAclDisconnectReq(transaction_id, bdaddr)` request
after an L2CAP close. EL71 implements it at Thumb address
`0xa0918886` and copies the six-byte address into its queued stack message.
The request is asynchronous, but it contains no accessor handle, so its
completion is not expected on this PoC's accessor. Before closing L2CAP the
PoC retains the connection's `BT_REMOTE_DEVICE_INFO`, submits the request, and
polls `BTA_RemBtDeviceInfo_GetStatus(accessor, device, 0)` until the status is
no longer `2` (ACL connected). Only then may it delete the accessor and ELF.
An L2CAP open failure does not issue an ACL disconnect, because that ACL may
be owned by another Bluetooth profile. This disconnect affects every profile
sharing the ACL link to that host.

If the peer closes L2CAP before the PoC's run timer expires, the disconnected
callback starts the same shutdown/finalize path immediately. Otherwise the
private `BTPAN` CEPID remains alive in `IDLE` state and incorrectly blocks the
next ELF launch even though ACL teardown already completed.

The process group is intentionally kept and reused on the next launch;
otherwise calling `CreateICL` twice for group `0x1a` would trigger the
firmware's `CrtIclResAlrdyExist` pickoff.

A second ELF instance must not kill an existing `0x1a00` process. Killing the
GBS process does not close its L2CAP channel and leaves the next connection
attempt racing the orphaned BNEP session. The launcher now refuses to replace
a live instance and exits immediately. A new test must wait for the previous
instance to print `BT NAP: kill_elf` after its asynchronous close completes.

## Remaining phone-as-NAP work

The final server path must:

1. Publish an SDP record with service class NAP (`0x1116`), L2CAP PSM
   `0x000f`, BNEP version/network packet type list, and Network Access Type.
2. Create/start an L2CAP `BTA_Server`, then enable accept with
   `BTA_Server_AcceptReq`.
3. Convert the now working client-side JSApi RX polling into the server data
   path. On a PANU setup request, validate destination NAP/source PANU and send
   the already implemented four-byte BNEP setup-success response.
4. Add DHCP (or static host setup) and forwarding/NAT as separate modules on
   top of the existing BNEP/ENIP data path.

The unresolved critical ABI is the firmware SDP record construction and RX
registration, not the BNEP encoder or outgoing L2CAP write.
