# Orchestra presets (chamber and symphony halls)

Three presets in the room library for orchestration. Each instrument **group** is one **mono** source: send the group's
mono signal to the input channel in the table. The listener is below the stage and faces the orchestra, so on the plan the
orchestra is **above** the listener, and left/right is the audience's left/right.

Presets:

| Preset | Hall | Listener |
|---|---|---|
| Chamber hall - audience, row 6 | 13 x 15 m, 9 m high, about 1.2 s decay, stage 0.8 m high with one riser | audience, row 6, 6.5 m from the stage |
| Symphony hall - audience, row 12 | 22 x 34 m, 17 m high, about 1.7-2 s decay, stage 1.1 m high with three risers | audience, row 12, 14 m from the stage |
| Symphony hall - conductor's podium | the same hall | the conductor's podium (ear at 1.6 m on a 0.3 m podium), facing the orchestra |

The stage and each riser are *plates* (horizontal surfaces at their own height) of parquet; over the stage there is a wooden
reflector. The audience floor is absorbing. The back wall has drapes.

## Symphony orchestra (17 inputs)

Seating as the audience sees it, left to right: first violins, second violins, violas, cellos, with the double basses behind
the cellos; woodwinds in the middle on the first riser; brass behind them on the second riser; timpani and percussion at the
back on the third riser. The distances are from the front edge of the stage.

| Input | Group | Family | Riser | Place (across, depth) |
|---|---|---|---|---|
| 1 | Violins I | strings, the highest | stage | left, 2.4 m |
| 2 | Violins II | strings | stage | left of centre, 2.9 m |
| 3 | Violas | strings, the middle | stage | right of centre, 2.9 m |
| 4 | Cellos | strings, low | stage | right, 2.8 m |
| 5 | Basses | strings, the lowest | stage | right, 5.3 m |
| 6 | Harp | | stage | far left, 4.9 m |
| 7 | Flutes | woodwind, high | 1 | left of centre, 6.2 m |
| 8 | Oboes | woodwind, nasal | 1 | centre, 6.3 m |
| 9 | Clarinets | woodwind, warm | 1 | centre, 6.3 m |
| 10 | Bassoons | woodwind, low | 1 | right of centre, 6.2 m |
| 11 | Horns | brass, mellow | 2 | left, 8.6 m |
| 12 | Trumpets | brass, bright | 2 | centre, 8.7 m |
| 13 | Trombones | brass, low | 2 | right of centre, 8.8 m |
| 14 | Tuba | brass, the lowest | 2 | right, 8.9 m |
| 15 | Timpani | drums (kettle drums) | 3 | left, 10.7 m |
| 16 | Percussion | cymbals, drums, bells | 3 | right, 10.8 m |
| 17 | Soloist | for a concerto, in front of the conductor | stage | centre-right, 1.0 m |

## Chamber orchestra (14 inputs)

| Input | Group | Riser |
|---|---|---|
| 1 | Violins I | stage |
| 2 | Violins II | stage |
| 3 | Violas | stage |
| 4 | Cellos | stage |
| 5 | Basses | stage |
| 6 | Flute | 1 |
| 7 | Oboe | 1 |
| 8 | Clarinet | 1 |
| 9 | Bassoon | 1 |
| 10 | Horns | 1 |
| 11 | Trumpets | 1 |
| 12 | Timpani | 1 |
| 13 | Harpsichord / piano | stage, in front |
| 14 | Soloist | stage, in front |

## Notes

- A group is a point, not a spread-out section. For a wider section, set the source to *Stereo* and send a stereo pair, or
  raise the group's spread in *Objects*.
- The sources face the audience with a slightly directional pattern (0.3); change *Directivity* per source in *Objects*.
- Everything can be moved: drag a group on the plan, or change its X / Y / Z.
