// Telemetry is consumed by its own widgets; never walk/stringify the document for meters.
export const viewportEvent = reason => ['snapshot', 'patch', 'parameters', 'view', 'diagnostics.paths', 'rays.paths', 'diagnostics.clutter', 'analysis.result', 'analysis.cleared', 'analysis.stale'].includes(reason);
