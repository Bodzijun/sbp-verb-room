import { button } from './dom.js';
export function historyButtons(store) {
  const make = (action, symbol) => {
    const name = action === 'undo' ? 'Undo' : 'Redo';
    const root = button(symbol, () => store.command({ op: `history.${action}` }), { 'data-action': action });
    const update = () => {
      const h = store.telemetry['history.changed'];
      const label = h?.[`${action}Label`];
      root.disabled = !store.ready || !h?.[action === 'undo' ? 'canUndo' : 'canRedo'];
      root.title = root.ariaLabel = `${name}${label ? ` · ${label}` : ''}`;
    };
    root.dispose = store.subscribe(({ reason }) => { if (['history.changed','snapshot','resync'].includes(reason)) update(); });
    update(); return root;
  };
  return [make('undo', '↶'), make('redo', '↷')];
}
