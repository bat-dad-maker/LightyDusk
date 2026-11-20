# Search: Link sinking in the field in BOTW

Date of consultation: **20/10/2026**, America/Sao_Paulo reference time.

Requested Scope: Search Switch emulators and BOTW port for Mac/iOS, documenting reports, solutions and relevance for this project. **Only search and this record: no code, settings, APK, saves or game files have been changed; no new test has been run on the mobile phone.**

## Main result

There are similar reports with at least two solutions confirmed by the authors themselves: disable **Reactive Flushing** in Eden and select **Dynarmic (JIT)** as CPU backend. This does not identify a unique cause for all cases and does not provide a patch directly applicable to our recompiler.

In the Mac/iOS port consulted I did not find an equivalent public report on the examined materials. This project uses **BOTW of Wii U**, not the Switch revision used here. The absence of reports in a recent project does not demonstrate absence of the defect.

The local records have already investigated part of the hypothesis of coherence CPU/GPU. The most specific local track remains the difference between the actor's height and the model's root, possibly related to the animation's ground/fit consultation. ** The local cause remains unconfirmed.**

## Comparative view

| Projeto/caso | Evidence found | Solution or documented situation | Applicability here |
| --- | --- | --- | --- |
| Eden, issue #672 | Pernas abaixo do terreno | Reactive Flushing desligado; autor confirmou | Compare memory coherence; do not copy configuration |
| Eden, user report | Stuck/deep in the grass | Dynarmic/JIT; autor confirmou | Investigating differences in CPU execution |
| Yuzu, relato de 2023 | Water sinking and collision | Full reading not available in this search | Historical track, no validated solution |
| Ryujinx, PR #3302 | Technical note mentions possible relationship with swimming in the air | Texture Synchronization Correction | Related precedent, not the same defect demonstrated |
| Sudachi, Suyu, Citron | Search for equivalent symptoms | I haven't located specific case verifiable with correction | Inconclusive result |
| Skyline/Strato e Ryubing | Search for equivalent symptoms | I haven't located specific case verifiable with correction | Inconclusive result |
| BotWRecomp Mac/iOS | README, documentation, patches and issues consulted | Nenhum relato equivalente encontrado | Different architecture and executable |
| Cemu, Wii U | Technical changelog and timestep documentation | Readback/cache/JIT and limitations above 30 FPS | Technical comparison, not Switch emulator |

“I haven’t located” means only a result of searches and sources accessible on the date, not a guarantee about the entire history of these projects. The sources and caveats of each line are below.

## 1. Eden: Reactive Flushing

Primary source: [issue #672 — pernas abaixo do terreno](https://github.com/eden-emulator/Issue-Reports/issues/672).

Report opened on 21/09/2026: BOTW Switch 1.6.0, Eden Nightly `df05d3de23`, Android, Adreno 830. The author observed the defect at approximately 30 FPS and reported that base 1.0.0 did not present it. Several driver changes and graphics adjustments have not resolved.

On 27/09 it was suggested to disable **Reactive Flushing**. On 28/09 the author confirmed the resolution; the issue is closed. It is a confirmed result in that environment, without causal patch identified in the report.

**Relevance:** prioritizes observing data coherence before assuming unique driver defect. ** Limit:** our case already occurs in Switch 1.0.0, and runtime is not Eden. Generically shutting down every readback does not necessarily equal the option of this issue.

## 2. Eden: backend de CPU

Primary source, user report and confirmation: [Link sinks when it stops](https://www.reddit.com/r/EdenEmulator/comments/1r6ni3m/in_zelda_breath_of_the_wild_link_sinks_into_the/).

Publication of 16/02/2026: the player describes Link sinking to cinnamon when stopping on the grass, but not on rock. On 02/05/2026 another user reports the same stop/stitch and recommends selecting **Dynarmic (JIT)** in advanced/debug configurations. The author confirms that it worked and updates the post.

**Strength of evidence:** Practical confirmation of users, without technical diagnosis of maintainer or controlled comparison of instructions. The previous backend was not documented precisely enough to state it was ECN. Do not complete “NCE bug” from this.

**Relevance:** the difference between terrain and rigid objects looks like the local record. It is a clue for CPU semantics, floating point and execution of ground queries. This static recompiler does not have an equivalent option to switch to Dynamic; the solution is not directly transferable.

## 3. Yuzu and derivatives: limits of evidence

The March 2023 account [“zelda botw - I'm sinking in the ground and water have no colission”](https://www.reddit.com/r/yuzu/comments/1236971/) was located. The search index presents the symptom and a comparison with Ryujinx, but the full page could not be recovered. **I do not use these excerpts to affirm the cause, solution or superiority of another emulator.**

Search also included Sudachi, Suyu, Citron, Skyline/Strato and Ryubing, combined with BOTW, Link, Feet, sinking, floor and ground. There was no specific report sufficiently verifiable with confirmed correction. Generic performance pages, TOTK reports and results unrelated to the problem were discarded.

There is no basis in this search to recommend installing a random fork or driver as a solution to our code.

## 4. Ryujinx: texture synchronization precedent

Source: [Original Mirrored Commit, PR #3302](https://git.axenov.dev/Museum/ryujinx/commit/d64594ec7476782a4254502dd0e36b8f1f49e381?show-outdated=&style=unified&whitespace=show-all), 29/04/2022. Additional consultation with [changelog preservado](https://git.renil.cc/renil/Ryujinx/wiki/Changelog.md), version 1.1.107.

The commit corrects the monitoring of texture synchronization actions: a flag could continue indicating a registered action after it was consumed; there was also a race in the record and problems with the numbering of the last sync. The author's comment mentions **possible** effect on BOTW episodes swimming in the air.

**Distinction:** correction exists, but the relationship with BOTW was described as possibility, not confirmation. Swimming in the air also does not demonstrate the same positioning failure of the model. The source is a mirror of the original author's code/historical; it is not a new investigation of the mirror maintainer.

As a clue to runtime, the discipline of invalidation, synchronisation consumed and rearmament is of interest. There is not enough evidence to apply this diff to this project.

## 5. Port of BOTW for Mac and iOS

Projeto identificado: [IshaanKalra2103/BotWRecomp](https://github.com/IshaanKalra2103/BotWRecomp).

Review examined: `2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d`, commit of 17/10/2026. The public API returned a commit and an issue, including open and closed, in the 20/10 query. That delimits a very small public history.

The [README of this review](https://github.com/IshaanKalra2103/BotWRecomp/blob/2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d/README.md) describes Recompilation **PowerPC → Wii U** C++, Runtime Cafe OS and Latte graphics layer of Cemu via Metal, for iPhone and Apple Silicon. Requires the updated US executable **v208**; the README calls it 1.6.0. Here the target is **ARM64 of Switch 1.0.0**, with NVN/Vulkan on Android. Version numbers between these platforms do not identify interchangeable executables.

Materiais consultados:

- [Public issues](https://github.com/IshaanKalra2103/BotWRecomp/issues) and [issue #1](https://github.com/IshaanKalra2103/BotWRecomp/issues/1): the only question returned about Android/open source, without reporting sinking.
- [iOS Notes](https://github.com/IshaanKalra2103/BotWRecomp/blob/2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d/ios/PORTING.md): describe testing on an iPhone, locking after a few minutes and working problems of the GPU in the background. I didn't find the defect of Link buried in them.
- [TODO](https://github.com/IshaanKalra2103/BotWRecomp/blob/2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d/todo.md) and [renderizador nativo experimental](https://github.com/IshaanKalra2103/BotWRecomp/blob/2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d/docs/native-renderer.md): the experimental renderer does not yet draw the whole world. This limitation is not equivalent to the searched bug.
- [Patches de FPS](https://github.com/IshaanKalra2103/BotWRecomp/blob/2b84b82dbe7efe41707d236fdbac9ec5e98c8d3d/runtime/src/game/fps.cpp) and `runtime/src/game/hooks.h`: there are pace/timestep adjustments and protection of certain cutscenes at 30 FPS. I didn't find a foot-specific/foot-IK/deepening patch in those files.

** Limited conclusion:** I found no equivalent report or solution published in the examined materials. It is not possible to attest, only by the README or by the absence of issues, that the port is free of this bug. PowerPC addresses and GX2 patches cannot be copied to our ARM64/NVN executable.

## 6. Cemu: historical comparison and timestep

Cemu emula **Wii U**, therefore this section is comparative.

The [changelog oficial 1.7.4](https://cemu.info/changelog/cemu_1_7_4.txt), with public launch on 09/04/2017, records experimental support reading textures back to the CPU, correction of corruption in the vertices/uniform cache and removal of an unsafe JIT optimization that could eliminate floating point writing. **Changelog does not explicitly associate one of these changes with Link.** It is not correct to conclude that the historical problem had a single GPU cause demonstrated by that list.

The [project's own wiki, FPS++ section](https://wiki.cemu.info/wiki/The_Legend_of_Zelda:_Breath_of_the_Wild#Issues_caused_by_FPS++_(Playing_over_30FPS)), documents timestep dependencies and physics/collition problems when exceeding 30 FPS. It recommends 30 FPS, static mode or disable FPS++ to separate these causes.

**Application:**control timestep is a good comparative future test, but the local record already reports persistent sinking with limitation to 30 FPS. The wiki text contains historical version guidelines; it does not constitute a specification of the Switch revision used here.

## 7. Confrontation with the evidence already recorded in this project

Local source: [BOTW-PORT.md](BOTW-PORT.md), especially the sinking link and sinking link and NPCs records, from 19–20/10/2026. The values below are **pre-existing ratings**, not new measurements of this research.

- The defect also affects elder/Bokoblin and varies according to the location. On a log, the contact of the feet was recorded as correct.
- There is comparison between actor position, `.hght` heights, GPU height array and decoded collision mesh. For the reference point, these heights are close to **214,352 m**.
- The root of the Link model was recorded in **213,798 m**, about **0.554 m below the actor**.
- History reports no effect tests with readback/writeback of certain render targets classes, synchronous graphical execution and 30 FPS limit.
- The CPU/GPU coherence annotations rule out specific paths and resources examined. They do not demonstrate that any form of synchronisation, synchronization or reading of the runtime GPU→CPU is correct.
- The differential tests of the lifter against Unicorn did not present failures in the annotated sets. This reduces the suspicion of errors covered by them, but does not prove correction of every execution sequence, FP state, competition or interaction with memory.

The local document contains different historical hypotheses, including an assignment to the terrain drawn followed by the finding on the model root. I didn't turn these steps into a unique causal conclusion. The most discriminating data is **actor/collition close to each other and model below**, if confirmed in new synchronized capture.

## 8. Hypothesis and possible future paths — not executed

This order is an evaluation of the search and local notes, not a list of proven corrections**.

| Prioridade | Hypothesis | Future observation that would distinguish the cause | Possible action only after confirmation |
| --- | --- | --- | --- |
| Alta | Ground query used by IK/root adjustment differs from physical controller contact | Compare in the same frame the height of the actor, root, feet and lightning results on ground and trunk | Fix return, filter, transformation or reading of the query that has been shown to be wrong |
| Alta | Difference between Havok and StaticCompound compressed terrain | Check hit/miss, radius fraction, position, normal and material in both surface classes | Correct the specific path of the query; do not raise the character globally |
| Medium | CPU/FP execution semantics on some uncovered path | Draw the first divergence in the chain that calculates vertical displacement; compare FPCR/FPSR, NaN, rounding and instructions involved | Fix operation or state preservation that diverges |
| Medium | Consistency, aliases or obsolete CPU/GPU data outside the already tested features | Identify a real consumer of outdated data and its expected synchronization | Adjust invalidation/readback/fence only on this path |
| Low, as repetition | Timestep/FPS as an exclusive cause | Measuring engine delta, not only FPS counter, on the same save/scene | Adjust engine time if there is a reproducible relation |
| Baixa inicialmente | Driver, shader cache or game version | A/B controlled with same input and supported revision | Investigate backend/drive or revision support if difference is demonstrated |

About the CPU clue: Dynarmic's report makes the investigation pertinent, but it does not show that our libter has the same error. About the graphical track: Eden's solution does not invalidate the documented local discards, nor does it authorize them to extrapolate to features that were not observed.

I do not recommend how to fix elevate Link for a constant, disable IK permanently, exchange dump for an unsupported revision or import Wii U patches. This can only mask the symptom, affect other surfaces/characters, or break code signatures.

## 9. Criteria for future validation

When there is permission to investigate or modify, a correction should be compared in the same scene/save on flat ground, slope, rock and static object; standing, walking, squatting, jumping and after recharge. Include Link and at least one affected NPC.

Register game review, runtime build, execution configuration, FPS and engine delta, actor coordinates, root matrix and relevant floor query. Visual success should follow consistency of this data and not create fluctuation, incorrect collision or displacement of other characters.

## 10. Traceability and limitations of the research

The references in items 1, 2 and 5 are reports/documentation of the projects or authors themselves; item 4 uses original code and commit message preserved by third parties. Item 6 uses Cemu's changelog and wiki. No download pages of games, caches or third party packages were used as technical evidence.

The requested issues API for Mac/iOS was [state=all](https://api.github.com/repos/IshaanKalra2103/BotWRecomp/issues?state=all&per_page=100); the history was [commits](https://api.github.com/repos/IshaanKalra2103/BotWRecomp/commits?per_page=100). The result refers to public visibility on that date, without access to Discord, private messages or Apple hardware testing.

Main terms: `Breath of the Wild Link sinking into ground`, `BOTW feet terrain`, `BOTW NCE Dynarmic`, `reactive flushing`, `Ryujinx air swimming texture sync`, in addition to combinations with emulator names and Mac/iOS port.

None of the external reports alone identifies the cause of this project's bug. This file records the evidence and next possible paths; ** does not apply a solution**.
