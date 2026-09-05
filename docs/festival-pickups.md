# Festival Tower pickups — build 86657

Walking over these props produces activity message 19, incident target 3539
(global SObject name `0x7A0FD954`, schema `0x808087F0`). Previously the activity
route validated and recorded that incident but awarded nothing. The deferred
client job queue **does drain in the same frame**; its flag is not an authority
gate. No opcode 601 request was observed for these placed pickups.

The four diagnostic pickups (three blue, then purple) identified the schema's
source hash, character/account identity, position and bubble. The source hashes
match packaged entity data at offset `0x638`:

| Placement list | Entity data tags | Sources |
| --- | --- | --- |
| Courtyard `80B4AF4F` | `80B4AF3F`, `46`, `48`, `4A`, `4C` | `E86DC713`, `E86DC710`, `E86DC711`, `E86DC716`, `B6D6DC59` |
| Bazaar `80B4AF36` | `80B4AF2E`, `30`, `32`, `34` | `E86DC717`, `E86DC714`, `E86DC715`, `B6D6DC5A` |

The project owner confirmed the intended server policy:

| Authored pickup | Reward |
| --- | --- |
| Courtyard blue (four sources) | **50 Candy** |
| Bazaar blue (three sources) | **60 Candy** |
| Purple in either tree | **250 Candy** |

All nine sources grant the installed Candy definition (`4084398230`). The server
looks up the individual authored source identifier; colour alone does not determine
the amount, and there is no random roll. The incident supplies an authored
source identity, never a trusted client-supplied item or quantity. Translated Tower
props retain their source hashes and are supported in the four Festival bubbles.

Follow-up inspection: all seven 1,760-byte blue entity-data definitions differ only
in self-reference handles and the hashes at `0x638`/`0x63C`. No payout difference
was recovered from those definitions. Direct inspection confirmed every payload
in the 6,416-row reward-mapping and 1,349-row reward-sheet tables is zeroed; the
1,625-row reward-item-list table has no payload fields. See the maintained manifest
research, `vault/sunrise/manifest/unlocks-rewards.md`, section 6. Reward amounts
therefore follow the owner's explicit Courtyard/Bazaar mapping, not an inferred
quantity field or an external description of another event version.

## Architecture and evidence boundary

This is compiled C++ **server behavior** reached through the authenticated BAP
activity-message route. It uses no gameplay scripts, script VM, position-polling
grant loop, or client-side inventory mutation. The periodic server connection poll
only delivers rewards already admitted from a real pickup event. The diagnostic
client loot probe is not required for this handler and grants nothing.

The implementation is a Festival-specific reconstruction. Observed evidence
establishes the incoming event, its source identifiers, and successful inventory
replication. It does **not** establish that Bungie's original server directly
granted from this event, nor reconstruct its full pending-loot/601 handshake.
Do not describe this as a recovered or complete retail loot protocol. The reward
table and claim lifetime are explicit Sunrise server policy.

The authenticated activity route checks ownership before the reward handler.
The handler checks the Tower destination, published Festival selection, account
and selected character, canonical placed-loot payload, and source allowlist.
Claims are deduplicated by activity generation, character, bubble and source.
The bounded queue retains an unpaid claim across subscriber/encoding failures.

An authenticated Family-4 subscriber prepares the standard profile acquisition,
stages its account update and notification, checks output capacity, then commits.
Only a successful commit advances the peer nonce/version and completes the claim.
Other account subscribers receive the existing account resync. Collections keep
their one-unit rule; server grants specify an explicit positive amount and still
obey the installed stack and bucket limits. No direct client inventory writes.

Validation on 2026-09-05:

- Release x64 build passed.
- `tests/loot_pickup_test.cpp`: four captured payloads, 320 truncated variants,
  malformed identity/subject/flags/padding, and nonfinite positions.
- `tests/festival_pickups_test.cpp`: hidden-event/wrong-account/wrong-destination
  rejection, duplicate suppression, encode/capacity/commit failures and retries,
  new activity generation, and all nine source payouts with duplicate checks.
  Covers Courtyard blue = 50, Bazaar blue = 60, and both purple sources = 250 Candy.
  Uses publication/state stubs.
- Earlier live test proved incident-to-inventory delivery with the former policy.
  It does not validate the final Courtyard/Bazaar amounts in game. Those amounts
  are covered by the updated regression test; a final live check remains separate.

Scope: this handles the authored Festival Tower bundles. It does not implement
general enemy loot or opcode 601. Claim history is process-local, and inventory
uses the existing State lifetime; no weekly reset/persistent claim system is added.
