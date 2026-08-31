# Technique boost trials — measurement protocol

Six rounds of static analysis on `psobb.exe` failed to locate the per-class technique boost (the whole
trail is in `psobb-client-map.md`). This is the plan to **measure** it in game instead.

Also published as an artifact for use during the session, with a printable recording sheet:
<https://claude.ai/code/artifact/614b7d25-5c79-4d19-93b1-735297c64e19>

## What is settled, and what is actually unknown

**Settled — technique level caps are NOT the mechanic.** They are server-side (`MaxTechLevels` in the
ItemPMT, 19 techs × 12 classes, stored as level−1) and **all four Force classes are identical on all 19
techniques**. Non-Forces cap at 15 (HUmar/RAmar) or 20 (HUnewearl/RAmarl).

**Settled — the boost exists and is a percentage.** The client's attack-technique damage formula carries
four boost terms, `CB/WB/FB/MB` = Class/Weapon/Frame/Mag, **added, not multiplied**, applied as roughly
`(X × MST) × 0.01` with `X` cached in entity fields `+0x116`/`+0x118`.

**Unknown:** which classes get `CB`, on which techniques, and how much. That is what this session is for.

## ⭐ Two measurements, because each is blind to a different boost shape

This is the part that must not be cut. Both come from the same casts.

| | Measurement A | Measurement B |
|---|---|---|
| **What** | ratios *inside* one character — `dmg(Rafoie) ÷ dmg(Foie)` | damage per MST *across* characters — `dmg(Foie) ÷ MST` |
| **Why it works** | MST is identical for both casts, so it cancels — no need to equalise stats across classes | normalises away the MST differences between classes |
| **Catches** | a family-specific boost (e.g. FOnewm on the Ra-line) | a flat class-wide boost |
| **Blind to** | ⚠ **a boost that lifts every technique equally** | nothing, but only as accurate as the recorded MST |

⚠ **Running only A is how this returns a false negative.** If a class boosts *everything* by a flat
amount, A cancels it and reports "no difference", which looks like a clean result.

## The control character — use HUnewearl

Of the four candidates, two would leave **holes in the control column**: HUmar cannot cast Shifta or
Deband, RAmar cannot cast Jellen or Zalure.

| Candidate | Tech cap | MST @ L200 | Cannot cast | Verdict |
|---|---|---|---|---|
| **HUnewearl** | 20 | **885** | — | **use this one** |
| RAmarl | 20 | 732 | — | fine backup, 17% less MST |
| HUmar | 15 | 594 | Shifta, Deband | gaps in the control column |
| RAmar | 15 | ~594 | Jellen, Zalure | gaps in the control column |

Higher MST is not vanity — bigger damage numbers make a small percentage boost visible above the
rounding.

⛔ **Grants, Megid and Reverser are Force-exclusive**, so no control exists for them. There the question
narrows to: do the four Forces agree with each other, or does one stand out?

## Setup — every item here is a term in the same formula as CB

- [ ] **Strip all equipment** — no weapon, armour, shield or units. Weapons and frames carry their own
      technique boosts; those are `WB` and `FB`, sitting right beside `CB`.
- [ ] **Remove the mag.** The mag boost is `MB`. ⚠ A mag left equipped is the most likely way to ruin
      the session.
- [ ] **No Shifta/Deband on the caster, no Jellen/Zalure on the target.**
- [ ] **Same technique level on every character** — set it explicitly with `$tech`, do not trust what a
      character happens to have learned.
- [ ] **Same enemy type, area and difficulty throughout.** Enemies carry per-element resistances
      (EFR `+0x2F6` / EIC `+0x2FA` / ELT `+0x2FE` are real entity fields), so changing target changes the
      answer.
- [ ] **Pick a target that survives the hit.** ⚠ If the enemy dies, displayed damage can be clipped to
      its remaining HP and the number is silently wrong.
- [ ] **Record each character's exact MST** from the stats screen, *after* stripping gear and mag.

## Procedure

1. **Determinism check.** Cast one technique five times on the same enemy type. If all five numbers are
   identical there is no RNG and **one cast per cell suffices** for everything after — roughly a
   fivefold saving. If they vary, take five per cell and use the median. Do this first.
2. **Baseline the control.** HUnewearl, stripped, all nine attack techniques at **level 15** (reachable
   by every candidate control, so the run survives a change of control). Record MST first.
3. **The four Forces**, same nine techniques, still level 15, same enemy and area. 9 × 5 = 45 cells.
4. **Repeat at technique level 1.** Two levels separate a *proportional* boost from a *flat* one; a
   single level cannot.
5. **Force-exclusive addendum.** Grants and Megid on all four Forces (Reverser does no damage — skip).

Techniques under test: Foie, Gifoie, Rafoie, Barta, Gibarta, Rabarta, Zonde, Gizonde, Razonde — plus
Grants and Megid for step 5.

## Reading the result

| Observation | Meaning |
|---|---|
| One Force's family sits above everyone else's (e.g. FOnewm's three Ra-techs) | The expected result. The percentage difference **is** `CB` for that class and family. |
| A shows nothing, but the Forces sit uniformly higher in B | A flat class-wide boost — precisely the case A cannot see. |
| Ratios differ at level 15 but not at level 1 | The boost is proportional, consistent with the percentage term in the formula. |
| Everything matches in both A and B | A real negative. Either the boost never reaches the damage number, or it is conditional on something removed during setup — **re-check the mag**. |

⚠ Whatever comes out is measured on **our** build and on **one enemy type**. Publish it as measured, not
as the general rule, until a second enemy type agrees.

## Recording the numbers

Reading damage off-screen across ~100 casts is slow and error-prone; the server sees every hit.

- **Run on Agrilat, never Coronet** — this needs temporary logging changes.
- **Raise command logging for the session only, then revert.** ⚠ Verbose `CommandData` is high-volume
  enough to rotate away the window being investigated — export the log immediately after, not the next
  day.
- Doubles as a **pilot for the item-drop logging** backlog item, which needs the same
  targeted-line-not-firehose approach.

## Why this is worth doing even for the RE route

A measured value does not just answer the question — it gives the static hunt something to recognise.
Knowing FOnewm's Ra-line carries (say) +30% turns *"find an unknown constant somewhere in five
megabytes"* into *"find 30 near a class comparison"*, which is a search that can actually succeed. The
two approaches became complementary once static scanning stalled.
