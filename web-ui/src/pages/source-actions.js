import { channels, pointCount } from '../state/selectors.js';
import { autoNumberSources } from '../viewport/t13.js';

export const canSum = source => channels(source) > 1;

export function sumState(sources) {
  const applicable = sources.filter(canSum);
  if (!applicable.length) return 'none';
  if (applicable.every(source => source.sum)) return 'on';
  if (applicable.some(source => source.sum)) return 'mixed';
  return 'off';
}

export function planSourceFormat(sources, format, available, maxPoints) {
  const proposed = sources.map(source => ({ ...source, format }));
  const points = list => list.reduce((total, source) => total + pointCount(source), 0);
  const fullRoute = autoNumberSources(proposed, available);
  const excessPoints = Math.max(0, points(proposed) - maxPoints);

  function place(indices) {
    const changed = new Set(indices);
    const retained = sources.filter((_, index) => !changed.has(index));
    const moving = indices.map(index => proposed[index]);
    if (points(retained) + points(moving) > maxPoints) return null;
    // Keep skipped sources on their original inputs, even when those inputs are outside the host bus.
    const reserved = retained.map(source => [source.firstIn, source.firstIn + channels(source)])
      .sort(([a], [b]) => a - b);
    if (reserved.some(([start], index) => index > 0 && start < reserved[index - 1][1])) return null;
    const route = autoNumberSources(moving, available, reserved);
    if (route.skippedSources) return null;
    return route.assignments.map(({ index, firstIn }) => ({ index: indices[index], firstIn }));
  }

  let indices = sources.map((_, index) => index);
  let route = place(indices);
  while (!route && indices.length) {
    indices.pop();
    route = place(indices);
  }
  // A skipped wide source need not prevent a later, narrower source from fitting.
  for (let index = indices.length; index < sources.length; index++) {
    const next = place([...indices, index]);
    if (next) {
      indices.push(index);
      route = next;
    }
  }
  const assignments = (route ?? [])
    .filter(({ index, firstIn }) => sources[index].format !== format || sources[index].firstIn !== firstIn)
    .map(assignment => ({ ...assignment, format }));
  // Native validates the point budget after each command as well as at commit.
  const delta = index => pointCount(proposed[index]) - pointCount(sources[index]);
  assignments.sort((a, b) => delta(a.index) - delta(b.index));
  return { assignments, skippedSources: sources.length - indices.length,
    missingChannels: fullRoute.missingChannels, excessPoints };
}

export function planSourceSum(sources, sum) {
  return sources.flatMap((source, index) => canSum(source) && source.sum !== sum ? [{ index, sum }] : []);
}

let transactionSerial = 0;
export async function applySourceBatch(store, label, assignments) {
  if (!assignments.length) return '';
  if (store.commandSlots < assignments.length + 3)
    return `${label} cancelled. Wait for the current source edits to finish.`;
  const id = `source-batch-${Date.now()}-${++transactionSerial}`;
  const send = (op, fields) => store.command({ op, ...fields }, id);
  // Enqueue the complete batch together so another gesture cannot commit it
  // between source edits. The store suppresses commit after any failed edit,
  // and suppresses cancel after a successful commit.
  const results = await Promise.allSettled([
    send('transaction.begin', { label }),
    ...assignments.map(assignment => send('source.setConfig', assignment)),
    send('transaction.commit'),
    send('transaction.cancel')
  ]);
  const [commit, cancel] = results.slice(-2);
  if (commit.value?.kind === 'ack') return '';
  if (cancel.status === 'rejected' || cancel.value?.kind === 'error')
    return `${label} failed. Could not confirm rollback.`;
  const failure = results.find(result => result.status === 'rejected' || result.value?.kind === 'error');
  return `${label} cancelled. ${failure?.reason?.message || failure?.value?.payload?.message || 'The source edit could not be completed.'}`;
}
