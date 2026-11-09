# Verb Room scene and preset work

Before creating or modifying scenes, acoustic materials, source placement or preset formats, read
[the scene-authoring guide](docs/Scene_authoring_agents_uk.md) and
[the preset exchange and Keep I/O guide](docs/Presets_0.1.10_uk.md).

Validate geometry, heights, material groups, input ranges and spatial-point budgets. Check impulse responses
and preset round-trips; successful parsing alone is not sufficient. Keep legacy scenes and project states compatible.

Use `.verbroom` for complete shareable presets; `.txt` describes a geometric scene. Presets do not configure
host sends or include instruments. Do not replace a VST3 binary while a host is using it, or close unsaved user projects.
