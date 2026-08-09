import { useEffect, useMemo, useState } from "react";
import { isLit, keyboardBindings, type KeyContext } from "@/device/keyboardMap";

const STORE_KEY = "stemtape.keyboardPanel.dismissed";

const CONTEXT_LABEL: Record<KeyContext, string> = {
  base: "base",
  fx: "fx overlay",
  heads: "heads",
  record: "record",
};

export function KeyboardPanel({
  heldKeys,
  fxOverlay,
  headsMode,
  globalScrub,
}: {
  heldKeys: readonly string[];
  fxOverlay: boolean;
  headsMode: boolean;
  globalScrub: 0 | 1 | -1;
}) {
  const [dismissed, setDismissed] = useState(true);

  useEffect(() => {
    setDismissed(window.localStorage.getItem(STORE_KEY) === "1");
  }, []);

  const context: KeyContext = headsMode ? "heads" : fxOverlay ? "fx" : "base";
  const bindings = useMemo(() => keyboardBindings(), []);
  const rows = useMemo(
    () => bindings.filter((b) => b.context === context || (context !== "base" && b.context === "record")),
    [bindings, context],
  );

  const close = () => {
    setDismissed(true);
    window.localStorage.setItem(STORE_KEY, "1");
  };
  const open = () => {
    setDismissed(false);
    window.localStorage.removeItem(STORE_KEY);
  };

  if (dismissed) {
    return (
      <button type="button" data-testid="keyboard-panel-open" className="st-kbd__reopen" onClick={open}>
        keyboard controls
      </button>
    );
  }

  return (
    <aside className="st-kbd" data-testid="keyboard-panel" data-context={context}>
      <header className="st-kbd__head">
        <span className="st-kbd__title">keyboard controls</span>
        <span className="st-kbd__context" data-testid="keyboard-panel-context">
          {CONTEXT_LABEL[context]}
        </span>
        <button type="button" aria-label="Dismiss keyboard controls" className="st-kbd__close" onClick={close}>
          ×
        </button>
      </header>
      {globalScrub !== 0 && (
        <p className="st-kbd__scrub" data-testid="keyboard-panel-shuttle">
          shuttling {globalScrub > 0 ? "forward" : "backward"} — all four stems
        </p>
      )}
      <ul className="st-kbd__list">
        {rows.map((b) => (
          <li key={`${b.id}:${b.codes.join("+")}`} className="st-kbd__row" data-lit={isLit(b, heldKeys) ? "1" : "0"}>
            <kbd className="st-kbd__keys">{b.label}</kbd>
            <span className="st-kbd__detail">{b.detail}</span>
          </li>
        ))}
      </ul>
    </aside>
  );
}
