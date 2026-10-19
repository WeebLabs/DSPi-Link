/*
 * The whole "framework": enough to build elements without a dependency.
 *
 * Bridge spec 10.4 rules out a UI framework, since every byte is stored in a
 * bridge's flash. Text always goes in through `text` (textContent), never as
 * markup, so names and SSIDs from the network cannot inject anything.
 */

/** Element properties for el(): `class`, `text`, `on<Event>` handlers, DOM properties or attributes. */
type Props = Record<string, unknown>;
/** A child for el() and append(); null, undefined and false stand for nothing. */
type Child = Node | string | null | undefined | false;

/**
 * Creates an element.
 *
 * @param tag The tag name.
 * @param props `class` sets className, `text` sets textContent, `onClick`
 *   style keys add listeners, known DOM properties are assigned and anything
 *   else becomes an attribute. Null, undefined and false are skipped.
 * @param children Nodes or strings to append; strings become text nodes.
 * @returns The new element.
 */
export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  props: Props = {},
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (value === undefined || value === null || value === false) continue;
    if (key === "class") node.className = String(value);
    else if (key === "text") node.textContent = String(value);
    else if (key.startsWith("on") && typeof value === "function") {
      node.addEventListener(key.slice(2).toLowerCase(), value as EventListener);
    } else if (key in node) {
      (node as unknown as Record<string, unknown>)[key] = value;
    } else {
      node.setAttribute(key, String(value));
    }
  }
  append(node, children);
  return node;
}

/**
 * Appends children, skipping the empty ones.
 *
 * @param parent Where to append.
 * @param children Nodes or strings; strings become text nodes.
 */
export function append(parent: Node, children: Child[]): void {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    parent.appendChild(typeof child === "string" ? document.createTextNode(child) : child);
  }
}

/**
 * Removes every child of a node.
 *
 * @param node The node to empty.
 */
export function clear(node: Node): void {
  while (node.firstChild) node.removeChild(node.firstChild);
}
