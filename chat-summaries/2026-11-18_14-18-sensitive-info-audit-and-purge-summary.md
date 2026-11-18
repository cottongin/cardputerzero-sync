# Sensitive Information Audit & Purge Summary

**Date:** 2026-11-18 14:18 EST
**Branch:** `master` (rewritten 15-commit history from `23db23a` to `59c3460`)

---

## Task Description

Audited the entire repository—including tracked source files, tests, documentation, specs/plans, chat summaries, untracked build/cache artifacts, `.deb` packaging output, and all git commits/reflogs—for passwords, API keys, local host workspace paths, and personal name/username/machine identifiers, then purged all identified occurrences using a full history rewrite (Option A) and clean artifact rebuild.

---

## Changes Made

1. **Sanitized 5 Tracked Files Across Full Git History:**
   - **[../docs/superpowers/plans/2026-11-18-cardputerzero-photo-sync.md](../docs/superpowers/plans/2026-11-18-cardputerzero-photo-sync.md):** Replaced local `file:///Users/<username>/...` spec link with relative link `../specs/2026-11-18-cardputerzero-photo-sync-design.md`.
   - **[2026-11-18_09-33-master-merge-and-docs-summary.md](2026-11-18_09-33-master-merge-and-docs-summary.md):** Replaced local `file:///Users/<username>/...` links with relative links (`../README.md`, `../docs/building-and-installation.md`, `../docs/usage-and-operations.md`), and updated referenced short SHAs to match the rewritten commits.
   - **[2026-11-18_09-15-cardputerzero-photo-sync-summary.md](2026-11-18_09-15-cardputerzero-photo-sync-summary.md):** Updated referenced short SHAs to match the rewritten commits.
   - **[../mac-receiver/tests/test_cli.py](../mac-receiver/tests/test_cli.py):** Replaced mock hostname `<first-name>-mac` / `<first-name>-mac.tail123.ts.net` with `my-mac` / `my-mac.tail123.ts.net`.
   - **[../device-ui/tests/test_ipc_and_theme.cpp](../device-ui/tests/test_ipc_and_theme.cpp):** Replaced mock paths `/Users/<first-name>/Pictures/CardputerZero` and `/Users/<first-name>/Photos` with `/Users/user/Pictures/CardputerZero` and `/Users/user/Photos`.
   - **[../device-ui/src/main.cpp](../device-ui/src/main.cpp):** Replaced sample simulator status path `/Users/<first-name>/Pictures/CardputerZero` with `/Users/user/Pictures/CardputerZero`.

2. **Rewrote Git History & Purged Unreachable Objects:**
   - Replayed all 15 commits on `master` onto a fresh orphan branch preserving original commit messages, timestamps, `cottongin <cottongin@users.noreply.github.com>` author/committer metadata, and GPG signatures (`-S`).
   - Replaced `master`, expired all reflogs (`git reflog expire --expire=now --all`), and ran `git gc --prune=now --aggressive` to permanently remove old git blobs/commits.

3. **Cleaned & Rebuilt Local Artifacts:**
   - Removed untracked `.superpowers/`, `device-ui/build/`, and `**/__pycache__/` directories (which contained local host paths, compiled `.pyc` path metadata, and ephemeral brainstorm tokens).
   - Ran Python test suites with `PYTHONDONTWRITEBYTECODE=1` and CMake/CTest builds in `/tmp/cp0-ui-test-build` (cleaned up afterward) so no local-path artifacts remain in the working tree.
   - Rebuilt `packaging/dist/cp0-sync_1.0.0_m5stack1_arm64.deb` inside the `linux/arm64` Docker container from the sanitized codebase.

---

## Verification

- `PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s packaging/tests -v`: `7/7` passed (`0.697s`, `OK`).
- `PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=mac-receiver python3 -m unittest discover -s mac-receiver/tests -v` (`BypassSandbox: true`): `24/24` passed (`6.415s`, `OK`).
- `PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=device-daemon:mac-receiver python3 -W error -m unittest discover -s device-daemon/tests -v` (`BypassSandbox: true`): `25/25` passed (`4.898s`, `OK`).
- `ctest --test-dir /tmp/cp0-ui-test-build --output-on-failure -V` (`BypassSandbox: true`, build dir removed afterward): `2/2` passed (`0.22 sec`).
- `./packaging/build-deb.sh` (`BypassSandbox: true`): Built clean `packaging/dist/cp0-sync_1.0.0_m5stack1_arm64.deb`.

---

## Lessons Learned

- Always use relative links (e.g., `../README.md`, `../specs/...`) instead of `file:///` absolute paths in committed plans, docs, and `chat-summaries/` files.
- Use generic placeholder paths and hostnames (`/Users/user/...`, `my-mac`) in test fixtures and sample simulator structs rather than personal identifiers.
- Run Python tests with `PYTHONDONTWRITEBYTECODE=1` and out-of-tree CMake build directories (`/tmp/...`) when keeping a repository working tree free of local host-path build caches.

---

## Follow-Up Items

- If the repository is later pushed to a remote or checked out on another machine, verify `git status` and `.gitignore` before committing build artifacts or running interactive brainstorm sessions.
