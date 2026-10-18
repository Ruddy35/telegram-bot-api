# Patches of TDLib

The server needs small changes of TDLib sources (`td/`). The submodule `td` points to the original commit of TDLib,
and the patches are applied to it when the build is configured.

| Patch | Changes |
|---|---|
| `0001-memory-object-counts.patch` | Adds the method `getMemoryObjectCounts`, which returns the numbers of files, chats, messages, users, media and other objects in memory of an instance, and allows bots to use `getStorageStatisticsFast`. Needed for the metrics `tgbotapi_bot_tdlib_objects`, `tgbotapi_bot_storage_bytes` and `tgbotapi_bot_storage_files` |

The patch only reads sizes of containers and maintains a counter of loaded messages. Except for allowing
`getStorageStatisticsFast` for bots, it doesn't change behavior of TDLib.

## How the patches are applied

CMake applies every `*.patch` in the order of names with `git apply` during configuration. An already applied patch
is skipped, so CMake can be run again. If a patch can't be applied, the configuration fails.

The applied patches change files inside the submodule `td`; `.gitmodules` sets `ignore = dirty` for it, so `git status`
doesn't show the submodule as modified because of them. Changes of the commit of the submodule are still shown.

`git` is needed. The patches are applied with a separate empty git directory in the build directory, so they are
applied in the same way whether `td` is a submodule, a plain directory or a copy without the parent `.git`, for
example, after `COPY .` in Docker.

Without `git` apply the patches manually and pass `-DTELEGRAM_BOT_API_APPLY_TD_PATCHES=OFF` to CMake:

```
cd td && for p in ../td-patches/*.patch; do patch -p1 < "$p"; done
```

The patches are checked only when CMake runs. If changes in `td/` are reverted, run CMake again; otherwise the build
fails because of the missing `getMemoryObjectCounts`.

## Updating TDLib

1. Revert the patches: `git -C td checkout .`
2. Update the submodule as usual.
3. Run CMake. If a patch can't be applied, change `td/` manually and recreate the patch, keeping the description at
   its beginning:

   ```
   git -C td diff > /tmp/diff && { sed '/^diff --git/,$d' td-patches/0001-memory-object-counts.patch; cat /tmp/diff; } > /tmp/patch \
     && mv /tmp/patch td-patches/0001-memory-object-counts.patch
   ```
