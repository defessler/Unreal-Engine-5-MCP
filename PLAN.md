# UE5_AI_BP — Plan

> Placeholder. The real plan lives in the user's earlier session output:
> `C:\Users\defes\AppData\Roaming\Claude\local-agent-mode-sessions\284e0ee8-36d6-479a-b058-4b46853e6cb3\94bafdef-0b2b-4109-8974-d9869688805e\agent\local_ditto_94bafdef-0b2b-4109-8974-d9869688805e\outputs\PLAN.md`
>
> Drop the full content in here when convenient. The repo-layout section needs one
> change vs. the original: there is no `Sandbox\` subfolder. The `.uproject` lives
> at the repo root, with the source-built engine as a sibling under `UnrealEngine\`.

## Repo layout (current)

```
D:\Projects\UE5_AI_BP\
├── UE5_AI_BP.uproject
├── Source\
│   ├── UE5_AI_BP.Target.cs
│   ├── UE5_AI_BPEditor.Target.cs
│   └── UE5_AI_BP\
│       ├── UE5_AI_BP.Build.cs
│       ├── UE5_AI_BP.h / .cpp           (IModuleInterface)
│       └── UE5_AI_BPGameModeBase.h/.cpp (stub)
├── Config\
│   ├── DefaultEngine.ini
│   ├── DefaultGame.ini
│   └── DefaultEditor.ini
├── Plugins\                              (empty — Phase 1: BlueprintReader)
├── mcp-server\                           (Phase 0 — not yet scaffolded)
├── UnrealEngine\                         (source-built engine, .gitignored)
├── PLAN.md
├── README.md
└── .gitignore
```

## Phases

- **Phase 0** — Standalone C++ MCP server at `mcp-server\` (not yet started).
- **Phase 1** — `Plugins\BlueprintReader\` UE plugin (not yet started).
- Both phases depend on a built engine — see `README.md` for the build steps.
