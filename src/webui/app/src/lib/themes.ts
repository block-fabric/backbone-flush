// The looks of the app: Midnight (the default, the look of chains-app), the themes of chains-qt
// (theme.cpp), as sets of CSS variables (styles/themes.css), plus the look of the desktop.
//
// A chain may have looks of its own (chain.json: exclusiveThemes), which the kit knows but offers only in
// the app of a chain that names them, and a look of a first start other than Midnight (chain.json:
// defaultTheme). Launchpad and Launchpad Day (styles/launchpad.css) are Flush's.
import { CHAIN } from '../chain/config';

export type ThemeId = 'system' | 'midnight' | 'light' | 'dark' | 'ocean' | 'matrix' | 'qdarkstyle' | 'mocha' | 'dracula' | 'launchpad' | 'launchpadday';

export interface Theme {
  id: ThemeId;
  name: string;
  /** Whether the theme is dark (for the colour scheme of form controls). */
  dark: boolean | 'auto';
  /** Offered only in the app of a chain that names it in chain.json (exclusiveThemes). */
  exclusive?: boolean;
  /** The family of looks it belongs to, beyond colours (root[data-look]: its own type, lines and shapes). */
  look?: 'launchpad';
  /** Its light (or dark) counterpart, for the quick switch of the top bars. */
  pair?: ThemeId;
}

/** Every look the kit knows (themes.css has the colours of each). */
export const ALL_THEMES: readonly Theme[] = [
  { id: 'system', name: 'Same as the desktop', dark: 'auto' },
  { id: 'launchpad', name: 'Launchpad', dark: true, exclusive: true, look: 'launchpad', pair: 'launchpadday' },
  { id: 'launchpadday', name: 'Launchpad Day', dark: false, exclusive: true, look: 'launchpad', pair: 'launchpad' },
  { id: 'midnight', name: 'Midnight', dark: true },
  { id: 'light', name: 'Light', dark: false },
  { id: 'dark', name: 'Dark', dark: true },
  { id: 'ocean', name: 'Ocean Teal', dark: true },
  { id: 'matrix', name: 'Matrix Green', dark: true },
  { id: 'qdarkstyle', name: 'Dark (classic Drivechain)', dark: true },
  { id: 'mocha', name: 'Catppuccin Mocha', dark: true },
  { id: 'dracula', name: 'Dracula', dark: true },
];

const OWN_THEMES: readonly string[] = CHAIN.exclusiveThemes ?? [];

/** The looks this app offers: those of the kit, and the chain's own (chain.json: exclusiveThemes) first. */
export const THEMES: readonly Theme[] = ALL_THEMES.filter((t) => !t.exclusive || OWN_THEMES.includes(t.id));

/** The look of a first start: the chain's (chain.json: defaultTheme), if this app offers it; else Midnight. */
export const DEFAULT_THEME: ThemeId = THEMES.find((t) => t.id === CHAIN.defaultTheme && t.id !== 'system')?.id ?? 'midnight';

export const MIN_FONT_ADJUST = -6;
export const MAX_FONT_ADJUST = 12;
export const BASE_FONT_PX = 14;

export function isTheme(value: unknown): value is ThemeId {
  return typeof value === 'string' && THEMES.some((t) => t.id === value);
}

/** The theme drawn for a choice: "system" follows the desktop. */
export function resolveTheme(id: ThemeId, prefersDark: boolean): Exclude<ThemeId, 'system'> {
  if (id !== 'system') return id;
  return prefersDark ? 'dark' : 'light';
}

/** Whether a choice is drawn dark. */
export function isDark(id: ThemeId, prefersDark: boolean): boolean {
  return THEMES.find((t) => t.id === resolveTheme(id, prefersDark))?.dark === true;
}

/** The quick switch of the top bar: a dark look goes light; the light one goes back to the dark look last used (Midnight at first). */
export function toggledTheme(id: ThemeId, prefersDark: boolean, lastDark: ThemeId | null): ThemeId {
  // A look with a counterpart (Launchpad, Launchpad Day) goes to it, and back.
  const pair = THEMES.find((t) => t.id === id)?.pair;
  if (pair && isTheme(pair)) return pair;
  if (isDark(id, prefersDark)) return 'light';
  return lastDark && lastDark !== 'light' && lastDark !== 'system' ? lastDark : DEFAULT_THEME;
}

export function clampFontAdjust(n: number): number {
  if (!Number.isFinite(n)) return 0;
  return Math.max(MIN_FONT_ADJUST, Math.min(MAX_FONT_ADJUST, Math.round(n)));
}

export type FontChoice = '' | 'inter' | 'nunito' | 'mono' | 'system';

/** The fonts that can go with any colours (the custom look of chains-qt: any colours with any font). */
export const FONTS: readonly { id: FontChoice; name: string }[] = [
  { id: '', name: 'The font of the theme' },
  { id: 'inter', name: 'Inter' },
  { id: 'nunito', name: 'Nunito' },
  { id: 'mono', name: 'JetBrains Mono' },
  { id: 'system', name: 'The font of the desktop' },
];

/** Put a look on the document: colours, a font apart from them, the font of amounts, the size. */
export function applyTheme(doc: Document, id: ThemeId, fontAdjust: number, prefersDark: boolean, font: FontChoice = '', moneyFont: 'embedded' | 'system' = 'embedded'): void {
  const resolved = resolveTheme(id, prefersDark);
  const root = doc.documentElement;
  root.dataset.theme = resolved;
  root.dataset.themeChoice = id;
  const theme = THEMES.find((t) => t.id === resolved);
  root.style.colorScheme = theme?.dark ? 'dark' : 'light';
  if (theme?.look) root.dataset.look = theme.look;
  else delete root.dataset.look;
  root.style.fontSize = `${BASE_FONT_PX + clampFontAdjust(fontAdjust)}px`;
  if (font) root.dataset.font = font;
  else delete root.dataset.font;
  root.dataset.moneyFont = moneyFont;
}
