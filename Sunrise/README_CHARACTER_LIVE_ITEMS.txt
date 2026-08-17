Sunrise - live character + named item editor patch
==================================================

What this patch changes
-----------------------
- Replaces the 0-255 character-level slider with a direct numeric input.
- Makes runtime AccountState authoritative for character/equipment edits. The UI mutates validated
  process State first instead of requiring a disk save before the change can exist in game.
- Adds live apply for mapped scalar character fields when a settled Queuez peer is available.
  Appearance value and Accepted remain runtime/persistence-only because no native replicated field
  has been identified for them yet.
- Mirrors metadata edits through partial, versioned Queuez updates: the selected Family-4 character
  object, that character's Family-3 record, and (when selected) that character's Family-0 record.
  The giant Family-4 account object is deliberately not resent for metadata-only edits.
- Checkpoints characters.dat only after the required Queuez companion-object chain succeeds. The
  same runtime character/equipment rows are then mirrored into settings.json as a compatibility
  copy; the rest of settings.json is retained and the replacement is parsed before installation.
  When no live Queuez peer exists, the runtime edit is checkpointed immediately instead.
- Adds item display names and a searchable, slot/class-filtered equipment chooser.
- Replaces an equipped item's definition in-place while preserving its existing item-instance SOID,
  level, and quantity. Native default sockets are regenerated for the new definition.
- Extends build-data extraction so catalogue items and their native default plugs are available for
  live swaps when they actually exist in the installed Destiny build.
- Adds catalogue subclasses to the ability-bucket extraction path so compatible same-class subclass
  swaps can resolve without inventing data.

Required item catalogue
-----------------------
This patch intentionally does not embed the third-party item database in the Sunrise binary.
Download this file:

  https://github.com/Loukie/d2loadouts/blob/main/data/items.js

and save it as:

  <Destiny/Sunrise module directory>\Sunrise\items.js

It must be beside Sunrise\characters.dat. Put it there BEFORE starting the game. Sunrise loads it at
startup and includes the selectable hashes in its installed build-data cache identity.

The linked catalogue describes weapons, armour, subclasses, and a broad hash-to-display-name map.
Sunrise still treats the installed game's own item/detail tables as authoritative: a catalogue item
that does not exist or cannot be resolved in this Destiny build is rejected instead of fabricated.

Install / build
---------------
1. Copy the files in this archive's Sunrise\ folder over the matching files in your current Sunrise
   source tree.
2. Put items.js in the runtime Sunrise folder described above.
3. Delete the old Release object directory if Visual Studio has stale objects:

     build\obj\x64\Release

4. Rebuild the x64 Release configuration.
5. Start Destiny/Sunrise normally.

The first startup with a newly-added or changed items.js hash set may take longer because Sunrise
must rebuild its installed item-detail cache.

Test checklist
--------------
1. Open the temporary Characters tab.
2. Confirm Level is now a numeric input rather than a slider.
3. Leave "Live apply character edits" enabled.
4. Change Human / Awoken / Exo and gender. Check:
   - the Sunrise roster row updates,
   - character-select/banner presentation updates,
   - if already in-world, whether the native player mesh also rebuilds without a transition.
5. Change Level and verify the replicated presentation updates without restarting Sunrise.
6. In Equipment, click a non-empty slot.
7. Search by item name (or hex/decimal hash) and choose a compatible result.
8. Confirm the row immediately shows the new name/hash and check the native inventory/equipment
   presentation without a restart.
9. Try a clearly incompatible slot/hash through the raw hash field: Sunrise should refuse it.

Expected catalogue startup log:

  ev=item_names result=ok options=... names=...

Expected live-refresh stages after a successful selected-character metadata edit:

  ev=queuez stage=editor_character4 result=ok ...
  ev=queuez stage=editor_roster result=ok reason=character_published ...
  ev=queuez stage=editor_banner result=ok reason=character_published ...
  ev=queuez stage=editor_checkpoint result=ok reason=saved ...

The checkpoint also emits State-side lines for both durable copies:

  ev=characters stage=save result=ok ...
  ev=characters stage=settings_mirror result=ok ...

For an inactive character, Family 4 and Family 0 are not resident; the editor starts with the
Family-3 character record instead.

Persistence model
-----------------
- settings.json remains the bootstrap/import document so existing Sunrise installs stay compatible.
- characters.dat is the durable runtime-character checkpoint and already stores explicit item socket
  lanes/plugs. That makes it suitable for perk editing without inventing a second item model.
- Once Sunrise is running, AccountState is the source used by Queuez encoders. Disk writes follow
  publication; they no longer gate whether a valid live mutation may enter runtime State.

Important current limits
------------------------
- Class changes still swap class-bound item instances and therefore change the resident Family-4
  object manifest. They are persisted, but this patch deliberately asks for a reselect/transition
  instead of faking resident identities.
- Create/delete/reset likewise change roster/item resident identities and still need a
  reselect/transition. In particular, deleting an inactive character removes that character's item
  objects from Family 4, so it is not a manifest-preserving live edit.
- Race/gender are mirrored in all three known live character records. If character-select/banner
  presentation becomes reliable but the already-spawned body/head still does not change, the next
  target is a missing native presentation/creation object rather than another broad account refresh.
- Appearance value and Accepted are stored by Sunrise but are not consumed by any traced Family-4,
  Family-3, or Family-0 encoder yet; their editor controls do not pretend to live-apply.
- Equipment editing currently replaces existing equipped instances; it does not create an item in
  an empty semantic slot.
