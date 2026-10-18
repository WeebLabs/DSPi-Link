/*
 * One confirmation dialog, built on the native <dialog> element, which gives
 * focus trapping, Escape to cancel and a backdrop without any code here.
 */

import { el } from "./dom.js";

export interface ConfirmOptions {
  title: string;
  /** Plain sentences saying exactly what will happen. */
  body: string[];
  confirmLabel: string;
  /** Red confirm button, for actions that erase something (spec 10.4). */
  danger?: boolean;
}

/** Shows the dialog and resolves true only if the user confirms. */
export function confirmAction(options: ConfirmOptions): Promise<boolean> {
  return new Promise((resolve) => {
    const cancel = el("button", { type: "button", text: "Cancel" });
    const confirm = el("button", {
      type: "button",
      class: options.danger ? "danger" : "primary",
      text: options.confirmLabel,
    });
    const dialog = el(
      "dialog",
      { class: "confirm", "aria-labelledby": "confirm-title" },
      el("h3", { id: "confirm-title", text: options.title }),
      ...options.body.map((sentence) => el("p", { text: sentence })),
      el("div", { class: "row end" }, cancel, confirm),
    );

    const close = (answer: boolean): void => {
      dialog.close();
      dialog.remove();
      resolve(answer);
    };
    cancel.addEventListener("click", () => close(false));
    confirm.addEventListener("click", () => close(true));
    dialog.addEventListener("cancel", (event) => {
      event.preventDefault(); // Escape: close through the same path.
      close(false);
    });

    document.body.appendChild(dialog);
    dialog.showModal();
    cancel.focus(); // The safe choice has focus, so Enter never erases anything.
  });
}
