/*
 * The settings page's DSPi connection section (bridge spec 10.3): which of the
 * bridge's pins are wired to the DSPi and how fast they talk, with a wiring
 * diagram drawn from the chosen pins. Saves through hub.config.set (Link spec
 * 7.10; keys in bridge spec 8.11).
 */

import { el } from "../dom.js";
import { statusLine } from "../status.js";
import { checkboxField, formatBaud, pinChoices, sameJson, selectField } from "./fields.js";
import { readBridgeConfig, type SectionContext, type UartConfig } from "./state.js";

/** Below this the bridge works, but slowly (bridge spec 5). */
const RECOMMENDED_MIN_BAUD = 921600;

/** The speed the pages recommend (bridge spec 5). */
const RECOMMENDED_BAUD = 1000000;

/** The DSPi's own UART pins, as DSPi Console sets them by default (bridge spec 5). */
const DSPI_TX_PIN = 16;
const DSPI_RX_PIN = 17;

/**
 * Draws the DSPi connection section from the state.
 *
 * @param context The page's context.
 */
export function drawDspi(context: SectionContext): void {
  const { state } = context;
  if (!state.config || !state.choices) return;
  const saved = state.config.uart;
  const draft = (): UartConfig => state.uartDraft ?? saved;
  const edit = (change: Partial<UartConfig>): void => {
    state.uartDraft = { ...draft(), ...change };
    refresh();
  };

  const diagram = el("div", { class: "wiring" });
  const hint = el("div", { role: "status" });
  const save = el("button", { type: "button", class: "primary", text: "Save" });
  const undo = el("button", { type: "button", text: "Undo" });
  const refresh = (): void => {
    const uart = draft();
    diagram.replaceChildren(wiringDiagram(uart.tx_pin, uart.rx_pin));
    hint.replaceChildren(...uartHints(uart));
    const changed = state.uartDraft !== null && !sameJson(state.uartDraft, saved);
    save.disabled = !changed || uart.tx_pin === uart.rx_pin || state.busy.has("dspi");
    undo.hidden = !changed;
  };

  save.addEventListener("click", () => void saveUart(context));
  undo.addEventListener("click", () => {
    state.uartDraft = null;
    context.draw("dspi");
  });

  context.add(
    "dspi",
    el("p", {
      class: "dim",
      text: "Which of the bridge's pins are wired to your DSPi, and how fast they talk.",
    }),
    diagram,
    ...uartFields(context, draft(), edit),
    hint,
    el("p", {
      class: "dim small",
      text: "Saving reconnects to the DSPi, so it drops out for a moment.",
    }),
    el("div", { class: "row" }, save, undo),
  );
  refresh();
}

/** The pin, speed and detection fields, showing `values` and reporting edits to `edit`. */
function uartFields(
  context: SectionContext,
  values: UartConfig,
  edit: (change: Partial<UartConfig>) => void,
): HTMLElement[] {
  const choices = context.state.choices?.uart ?? { tx_pin: [], rx_pin: [], baud: [] };
  return [
    el(
      "div",
      { class: "fields" },
      selectField(
        "uart-tx",
        "Bridge TX pin (to the DSPi's RX)",
        pinChoices(choices.tx_pin),
        values.tx_pin,
        (value) => edit({ tx_pin: Number(value) }),
      ),
      selectField(
        "uart-rx",
        "Bridge RX pin (from the DSPi's TX)",
        pinChoices(choices.rx_pin),
        values.rx_pin,
        (value) => edit({ rx_pin: Number(value) }),
      ),
    ),
    selectField(
      "uart-baud",
      "Baud rate",
      choices.baud.map((baud) => ({ value: baud, label: formatBaud(baud) })),
      values.baud,
      (value) => edit({ baud: Number(value) }),
      "The speed of the link to your DSPi. It must match the DSPi's UART setting in DSPi Console.",
    ),
    checkboxField(
      "uart-detect",
      "Find the speed by itself if the DSPi doesn't answer",
      values.detect_baud,
      (checked) => edit({ detect_baud: checked }),
    ),
  ];
}

/** What is wrong with, or worth knowing about, the chosen pins and speed. */
function uartHints(uart: UartConfig): HTMLElement[] {
  if (uart.tx_pin === uart.rx_pin) {
    return [statusLine("bad", "TX and RX must be different pins.")];
  }
  if (uart.baud < RECOMMENDED_MIN_BAUD) {
    return [
      statusLine(
        "warn",
        "This speed is slow",
        "The bridge works, but reads take longer and meters update less often. " +
          `${formatBaud(RECOMMENDED_BAUD)} is recommended.`,
      ),
    ];
  }
  return [];
}

async function saveUart(context: SectionContext): Promise<void> {
  const { client, state } = context;
  const change = state.uartDraft;
  if (!change) return;
  await context.run("dspi", async () => {
    const config = readBridgeConfig(await client.setHubConfig({ uart: change }));
    if (!config) throw new Error("the bridge's reply could not be read");
    state.config = config;
    state.uartDraft = null;
  });
}

// ---------------------------------------------------------------------------
// The wiring diagram

const SVG = "http://www.w3.org/2000/svg";
const DIAGRAM_WIDTH = 360;
const DIAGRAM_HEIGHT = 150;
const BOARD_WIDTH = 128;
const BOARD_INSET = 4;
const BOARD_CORNER = 10;
const BOARD_LABEL_Y = 24;
/** How far a pin's name sits inside its board's edge. */
const PIN_LABEL_INSET = 18;
const FIRST_WIRE_Y = 46;
const WIRE_SPACING = 34;
/** Text sits on its baseline; this centres it on the wire. */
const PIN_LABEL_DROP = 4;
const WIRE_LABEL_RISE = 6;

/** An SVG element with its attributes. */
function svg(tag: string, attributes: Record<string, string | number>, text?: string): Element {
  const node = document.createElementNS(SVG, tag);
  for (const [name, value] of Object.entries(attributes)) node.setAttribute(name, String(value));
  if (text !== undefined) node.textContent = text;
  return node;
}

/**
 * The three wires between the bridge and the DSPi, with the chosen pins. The
 * bridge's TX goes to the DSPi's RX and the other way round; that crossing is
 * the mistake people make most, so the drawing shows it. Built from DOM nodes
 * with textContent, so nothing from the bridge is ever parsed as markup.
 */
function wiringDiagram(txPin: number, rxPin: number): Element {
  const rows: Array<[string, string, string]> = [
    [`GPIO ${txPin} (TX)`, `GPIO ${DSPI_RX_PIN} (RX)`, "Bridge sends"],
    [`GPIO ${rxPin} (RX)`, `GPIO ${DSPI_TX_PIN} (TX)`, "DSPi sends"],
    ["GND", "GND", "Ground"],
  ];
  const rightBoardX = DIAGRAM_WIDTH - BOARD_INSET - BOARD_WIDTH;
  const boardHeight = DIAGRAM_HEIGHT - 2 * BOARD_INSET;
  const board = (x: number): Element =>
    svg("rect", {
      x,
      y: BOARD_INSET,
      width: BOARD_WIDTH,
      height: boardHeight,
      rx: BOARD_CORNER,
      class: "board",
    });
  const boardLabel = (x: number, text: string): Element =>
    svg(
      "text",
      { x: x + BOARD_WIDTH / 2, y: BOARD_LABEL_Y, class: "board-label", "text-anchor": "middle" },
      text,
    );

  const drawing = svg("svg", {
    viewBox: `0 0 ${DIAGRAM_WIDTH} ${DIAGRAM_HEIGHT}`,
    role: "img",
    "aria-label": `Bridge GPIO ${txPin} to DSPi RX, bridge GPIO ${rxPin} to DSPi TX, ground to ground`,
  });
  drawing.append(
    board(BOARD_INSET),
    board(rightBoardX),
    boardLabel(BOARD_INSET, "Bridge"),
    boardLabel(rightBoardX, "DSPi"),
  );
  rows.forEach((row, index) => drawing.append(...wire(row, index, rightBoardX)));
  return drawing;
}

/** One wire of the diagram: the pin names at each end and what the wire carries. */
function wire(
  [left, right, carries]: [string, string, string],
  index: number,
  rightBoardX: number,
): Element[] {
  const y = FIRST_WIRE_Y + index * WIRE_SPACING;
  const labelY = y + PIN_LABEL_DROP;
  return [
    svg("text", { x: PIN_LABEL_INSET, y: labelY, class: "pin" }, left),
    svg(
      "text",
      { x: DIAGRAM_WIDTH - PIN_LABEL_INSET, y: labelY, class: "pin", "text-anchor": "end" },
      right,
    ),
    svg("line", {
      x1: BOARD_INSET + BOARD_WIDTH,
      y1: y,
      x2: rightBoardX,
      y2: y,
      class: `wire wire-${index}`,
    }),
    svg(
      "text",
      {
        x: DIAGRAM_WIDTH / 2,
        y: y - WIRE_LABEL_RISE,
        class: "wire-label",
        "text-anchor": "middle",
      },
      carries,
    ),
  ];
}
