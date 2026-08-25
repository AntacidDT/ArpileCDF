# LOFS — Lost & Found Solutions

LOFS is **Arpile 32CDF's** debugging and incident knowledge base. Its name is a
reminder: solutions that live only in chat history are *lost*; LOFS makes sure
they are *found* again.

## Purpose

Every non-trivial problem, error, investigation, false lead, root cause, fix,
and verification result gets permanently documented here, so debugging
knowledge survives beyond the conversation in which it was discovered.

Entries preserve **not only the final solution**, but also:

- the important **failed hypotheses** (so nobody re-tests them next time),
- the **diagnostic discoveries** made along the way,
- the exact **fix**, and
- how it was **verified** on real hardware.

## Entry format

Each entry lives in `entries/` as `LOFS-XXX-short-name.md` and uses this
structure:

```markdown
# LOFS-XXX — Short Problem Name

- **Status:** SOLVED / OPEN / WORKAROUND / WONTFIX
- **Severity:** Critical / High / Medium / Low
- **Subsystem:** ...
- **Date:** YYYY-MM-DD

## Symptom

What was observed.

## Initial Hypotheses

What we suspected at the beginning.

## Investigation

Important tests, observations, commands, logs, and reasoning.

## False Leads

Things we investigated that turned out not to be the root cause.

## Root Cause

The actual cause once established.

## Fix

Exactly what was changed.

## Verification

How we proved the fix works.

## Lessons Learned

What should be remembered to avoid repeating the problem.
```

## Rules

1. IDs are sequential (`LOFS-001`, `LOFS-002`, ...) and never reused.
2. Every new entry must be added to [INDEX.md](INDEX.md).
3. Update an entry's **Status** when reality changes (e.g. WORKAROUND → SOLVED).
4. Be honest about false leads — they are the most valuable part of the record.
5. Prefer exact file paths, function names, error strings, and log lines over
   vague descriptions.

## Layout

```text
lofs/
├── README.md      <- this file
├── INDEX.md       <- table linking every entry
└── entries/       <- one markdown file per incident
```
