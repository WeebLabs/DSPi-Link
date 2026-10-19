/*
 * One confirmation dialog, built on the native <dialog> element, which gives
 * focus trapping, Escape to cancel and a backdrop without any code here.
 * Bridge spec 10.4 asks for every destructive action to be confirmed; this is
 * where both pages do it.
 */

import { el } from "./dom.js";

/** What the dialog says. */
export interface ConfirmOptions {
  title: string;
  /** Plain sentences saying exactly what will happen. */
  body: string[];
  confirmLabel: string;
  /** Red confirm button, for actions that erase something (bridge spec 10.4). */
  danger?: boolean;
}

let dialogCount = 0;

/**
 * Shows the dialog and waits for the user.
 *
 * @param options The wording.
 * @param signal Closes the dialog as cancelled, for a page that goes away
 *   while it is open.
 * @returns True only if the user confirms.
 */
export function confirmAction(options: ConfirmOptions, signal?: AbortSignal): Promise<boolean> {
  if (signal?.aborted) return Promise.resolve(false);
  // Safari before 15.4 has no <dialog>; the browser's own box says the same.
  if (typeof HTMLDialogElement !== "function" || !("showModal" in HTMLDialogElement.prototype)) {
    return Promise.resolve(window.confirm([options.title, ...options.body].join("\n\n")));
  }
  return new Promise((resolve) => {
    const titleId = `confirm-title-${++dialogCount}`;
    const cancel = el("button", { type: "button", text: "Cancel" });
    const confirm = el("button", {
      type: "button",
      class: options.danger ? "danger" : "primary",
      text: options.confirmLabel,
    });
    const dialog = el(
      "dialog",
      { class: "confirm", "aria-labelledby": titleId },
      el("h3", { id: titleId, text: options.title }),
      ...options.body.map((sentence) => el("p", { text: sentence })),
      el("div", { class: "row end" }, cancel, confirm),
    );

    const close = (answer: boolean): void => {
      signal?.removeEventListener("abort", onAbort);
      dialog.close();
      dialog.remove();
      resolve(answer);
    };
    const onAbort = (): void => close(false);
    cancel.addEventListener("click", () => close(false));
    confirm.addEventListener("click", () => close(true));
    dialog.addEventListener("cancel", (event) => {
      event.preventDefault(); // Escape: close through the same path.
      close(false);
    });
    signal?.addEventListener("abort", onAbort);

    document.body.appendChild(dialog);
    dialog.showModal();
    cancel.focus(); // The safe choice has focus, so Enter never erases anything.
  });
}
