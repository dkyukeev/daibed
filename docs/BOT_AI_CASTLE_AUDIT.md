# Castle bot AI: navigation and decision audit

## Implemented and reproduced

- Opening pressure previously selected a defender inside a 14-block base
  radius. Outside it, defense resumed, reversing movement. The opening policy
  now excludes defenders and lets an existing attacker continue its departure.
- Strategic graph entry previously used geometric proximity alone. Travelling
  bots now verify a physical route to one of the four nearest accessible
  portals, with bounded action searches and no construction or mining. This
  preserves the intended bridge crossing instead of selecting its disconnected
  landing as the entry point.
- The three-block descent limit trapped bots on Castle roofs; reducing it
  after a route failure made recovery worse. Runtime navigation now permits
  verified landings up to eight support blocks below. Player physics has no
  height-based fall damage. The real command/physics smoke descends from
  (86,60,-10) to the entrance floor at (87,52,-10).
- A partial-bridge fixture checks that an empty-handed builder selects the
  connected departure portal. It models construction on the map; it is not a
  saved snapshot of every block from the original failing match.

## Controlled evaluation

Release diagnostics build; Castle, Hard, Hypixel Rush, Ruins, initial seed
10101; two capped six-minute matches. Compare `build/castle_opening_2x6.json`
(opening policy fix) with `build/castle_entry_budget_2x6.json` (verified entry
and roof descent).

| Metric, summed over two runs | Opening fix | Entry + descent |
|---|---:|---:|
| Movement failures | 157 | 145 |
| Navigation stuck events | 76 | 73 |
| Local path search failures | 857 | 3 |
| Corridor failures | 206 | 1782 |
| Kills | 0 | 6 |
| Void falls | 5 | 13 |
| Core damage | 152 | 150 |
| Naturally destroyed cores | 1 | 1 |
| Strategic stall samples | 5194 | 4844 |

The reduced local failure count is NOT an equivalent reduction in overall
navigation failures: entry rejection moves failures to the corridor layer.
Entry probe searches are not included in local path-request counters. In the
latest run, first core damage occurred at 325.381 seconds in match one; match
two had none. This is mixed evidence, not demonstrated human-level strength.

## Remaining priorities

### Recovery follow-up

The same pinned two-match experiment, comparing
`build/castle_entry_budget_2x6.json` with
`build/castle_recovery_stable_2x6.json`:

| Metric | Before recovery ownership | After |
|---|---:|---:|
| Void falls | 13 | 5 |
| Movement failures | 145 | 130 |
| Navigation stuck events | 73 | 94 |
| Local path search failures | 3 | 362 |
| Corridor failures | 1782 | 2478 |
| Strategic stall samples | 4844 | 5154 |
| Core damage / cores destroyed | 150 / 1 | 150 / 1 |
| Kills | 6 | 5 |

This addresses unsafe fallback ownership, not general offensive strength.
Safety improved in these runs; stalls and search failures remain worse than
the preceding version. One match again had no core damage. New Game-level
smokes cover partial approach, retention across retry cooldown, stopping at
the supported edge, rejoining after bridge construction, holding during a
failed search, and distinct adjacent recovery goals.

1. No-entry recovery now retains navigation ownership: an executable partial
   approach or braking replaces blind legacy steering. Local search cooldowns
   also retain ownership. Approaches persist until arrival or 12 seconds
   without progress, after which failure evidence reaches strategic planning.
   A rendezvous helper is now requested when the blocked bot has fewer than
   four blocks. Resource transfer and verification that the requester resumed
   its mission remain open.
2. Entry failures on dynamically built bridges: remaining recorded examples
   hit ActionLimit, including (-25,54,-72) and (-57,53,-33). These need captured
   dynamic geometry and bounded recovery tests; increasing search limits alone
   does not establish connectivity.
3. Assault completion: successful travel is not reliably converted into core
   damage. Inspect interruptions, bridge stock and defense-breaking ownership
   during the final approach.
4. Human parity requires reproducible matches against human players or a
   stronger fixed opponent, across multiple seeds. Bot-only capped openings
   establish regressions and specific behaviors, not competitive parity.

## Helper follow-up

Fixed repeated requests clearing their selected builder, attack targeting
overwriting the helper destination, and helper goals treating an interaction
cell inside the enemy Core as a standable destination. Requests now rendezvous
at the stranded bot. Progress renews the assignment; stalled or empty-handed
builders can be replaced. Existing retreat/recovery decisions and the base
defender take priority. Only the owner can complete a request.

Same pinned two-match comparison: `build/castle_recovery_stable_2x6.json`
versus `build/castle_help_rendezvous_2x6.json`:

| Metric | Recovery version | Helper version |
|---|---:|---:|
| Kills | 5 | 12 |
| Void falls | 5 | 6 |
| Core damage / cores destroyed | 150 / 1 | 140 / 1 |
| Strategic stall samples | 5154 | 4025 |
| Movement failures | 130 | 172 |
| Navigation stuck events | 94 | 103 |
| Local path search failures | 362 | 673 |
| Corridor failures | 2478 | 2051 |

The helper version records 51 assignments/reassignments and 10 arrivals;
these are not 51 independent missions or 10 proven rescues. Both matches have
core damage, first at 251.049 and 358.940 seconds, but damage remains late.
Combat activity and strategic stalls improved in these samples; navigation
failures and void falls did not. No human-parity claim follows from this test.
The next iteration below addresses helper selection and observes whether the
requester actually resumes progress.

Release build, 36 bot-policy smoke scenarios and the navigation suite passed.
The navigation suite includes island entry rejection, partial bridge return,
Castle roof descent through real commands, the Castle exit lane, and a helper
building across a gap to its request destination through authoritative commands.

## Helper nomination and observed outcomes

Team coordination now nominates a helper before the individual bot update loop.
Candidates must be alive, have building stock, and not be defending, retreating,
recovering, or engaged with a nearby visible enemy. A bounded voxel search
evaluates the approach with the actual inventory and movement tuning. Complete
routes outrank partial approaches; estimated travel time and spare blocks rank
the remaining candidates, with player ID breaking ties independently of update
order. Partial approaches remain explicitly tentative. The active progress lease
continues to protect a travelling builder from reassignment.

After arrival, observations distinguish grounded progress of at least four
blocks toward the requester's objective from death, changed objective, or no
result in 30 seconds. Arrival alone is not success. Repeated requests from that
requester are suppressed during observation; requests from other teammates
remain available. These observations show temporal association, not proof that
the helper caused the progress.

Final pinned comparison: `build/castle_help_rendezvous_2x6.json` versus
`build/castle_help_observation_2x6.json` (Castle, Hard, Hypixel Rush, Ruins,
seed 10101, two six-minute runs, speed 256):

| Metric | Previous helper version | Nomination and observation |
|---|---:|---:|
| Kills | 12 | 7 |
| Void falls | 6 | 5 |
| Core damage / cores destroyed | 140 / 1 | 150 / 1 |
| Strategic stall samples | 4025 | 4799 |
| Movement failures | 172 | 164 |
| Navigation stuck events | 103 | 119 |
| Local path search failures | 673 | 422 |
| Corridor failures | 2051 | 2350 |

The final runs recorded four helper assignments, three arrivals, one objective
change, one unresolved observation and one observation still pending at match
timeout. There were no observed successful resumptions. Only the second run
damaged a Core, first at 324.815 seconds. This is not an overall competitive
improvement: fewer local search failures coexist with more corridor failures
and stalls. The intermediate `castle_help_selection_2x6.json` pilot exposed
repeated ineffective rendezvous and was superseded by the observation window.

Release build, all 42 bot policy scenarios, and the navigation suite passed.
The suite now also invokes production team nomination on separated platforms
and verifies that reversing player order does not replace the reachable helper.

Remaining strategic problem: reaching a stranded ally does not necessarily open
its onward route. Assistance needs a verified exit-building or resource-handoff
task, followed by measured resumption. Sustained early attacks and human-match
evaluation remain necessary before claiming parity with live players.

Long-run validation: `build/castle_help_observation_full.json`, same settings
and seed with a 30-minute limit, completed without timeout at 1745.6 seconds
(29:06), winner team 0. It recorded 12 kills, 256 Core damage and nine void
falls. First Core damage occurred at 384.284 seconds; the timeline attributes
Core destructions at 394.414 and 494.040 seconds to bot attacks. The aggregate
four destroyed Cores must not be reported as four attacking successes. Two
helper arrivals produced one changed-objective and one unresolved outcome,
with no observed resumption. Finishing this match validates match completion,
not fast offensive play or competitiveness against humans.

## Building-stock handoff

An arrived helper can now drop 4–16 blocks for an understocked teammate using
the ordinary, exactly-once `DropItem` economy command. Both players must be
grounded, on the same team, within two blocks, and have line of sight. The donor
retains four blocks of its selected building material; a recipient with at least
four blocks is not supplied. Items travel and are collected through normal
pickup physics, so a drop is not reported as a confirmed pickup or rescue.
`bridgeHelpSupplied` records successful drops; outcome observation still checks
the recipient's subsequent movement separately.

The navigation integration suite verifies that 20 donor blocks become four
donor and 16 recipient blocks, a duplicate command cannot duplicate the drop,
and the recipient uses seven blocks through authoritative placement to reach
the neighboring platform approach. It also checks the donor reserve and an
already-restocked recipient. All navigation checks and 42 bot policy scenarios
passed, with a successful Release build.

Pinned Castle comparison, same two six-minute runs and settings as above:
`build/castle_help_observation_2x6.json` versus
`build/castle_help_supply_2x6.json`:

| Metric | Observation version | Stock handoff |
|---|---:|---:|
| Kills | 7 | 9 |
| Core damage / cores destroyed | 150 / 1 | 234 / 1 |
| Void falls | 5 | 4 |
| Strategic stall samples | 4799 | 4675 |
| Movement failures | 164 | 124 |
| Navigation stuck events | 119 | 94 |
| Local path search failures | 422 | 345 |
| Corridor failures | 2350 | 2287 |

Four assignments produced three arrivals and two successful block drops. Two
observations ended with changed objectives and one remained unresolved; none
met the retained-objective progress criterion. Both runs damaged a Core, first
at 319.200 and 324.815 seconds. These samples improve several counters but do
not establish general competitive strength or successful Castle rescues. The
end-to-end rescue is verified in the controlled navigation fixture. Remaining
work includes stocked bots rejoining the authored network from partial bridges,
and distinguishing useful replanning after supply from ineffective goal changes.

## Hypothesis: stocked bots cannot repair route entry

Confirmed in a controlled production-navigation reproduction. A bot on a short
unfinished bridge was unable to join the authored graph even after receiving
16 blocks: the selected recovery target remained its current position. The
existing entry search prohibited placement, and recovery execution prohibited
bridging. `build/entry_restock_repro.log` records the failing scenario before
the fix (`target=0`, `x=0`).

Entry selection now retains the existing-terrain search as its first choice.
Only when it fails does a second bounded search consider a complete repair to
a real graph entrance. It checks at most four entrances, 3200 expansions each,
with at most eight consecutive bridge blocks and a two-block reserve. Mining
and stair building remain disabled for this repair. Partial construction paths
are rejected. An explicit recovery flag permits bridging only for the selected
repair; normal recovery retains its existing restrictions. On reaching the
entrance the bot searches the now-connected graph again. Entry planning also
uses the bot's terrain speed and jump tuning.

`build/entry_repair_verified_navigation.log` verifies that the empty-handed bot
waits, four blocks cannot start the incomplete crossing, and 16 blocks permit
actual command-driven construction and graph re-entry (`target=9`, `x=8.024`).
The navigation suite and all 42 bot-policy scenarios passed, as did Release
compilation and the whitespace check.

The broader hypothesis that this immediately accelerates Castle play is **not
confirmed**. Pinned comparison, same settings and seed as previous iterations:
`build/castle_help_supply_2x6.json` versus `build/castle_entry_repair_2x6.json`:

| Metric | Stock handoff | Entry repair |
|---|---:|---:|
| Kills | 9 | 9 |
| Core damage / cores destroyed | 234 / 1 | 234 / 1 |
| Void falls | 4 | 4 |
| Strategic stall samples | 4675 | 4858 |
| Movement failures | 124 | 108 |
| Navigation stuck events | 94 | 83 |
| Local path search failures | 345 | 345 |
| Corridor failures | 2287 | 2279 |

The timeline records two repair selections for actor 6 at 124.562 and 125.829
seconds, both near support (-56,53,-35). Selection is not completion: the current
timeline does not establish whether those two repairs finished. Core attack
timings are unchanged (319.200 and 324.815 seconds), and strategic stalls rose.
There were two helper arrivals and one supply drop, with no retained-objective
progress observation. Next investigation should distinguish interrupted repair
tasks from completed re-entry and examine why strategic goals change during
recovery; this sample does not establish overall competitive improvement.
