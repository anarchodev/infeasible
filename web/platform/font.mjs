// font.mjs — the two built-in glyph sets, one per frozen text cell.
//
// §12 freezes text METRICS, not letter shapes: `print` advances a fixed cell
// per character on every backend, so a native player may substitute its own
// font without any cart's UI shifting by a pixel. These are the Canvas2D
// backend's, and they are bitmap rather than `fillText` for one reason: text
// renders at internal resolution like everything else, or the aesthetic splits
// into game pixels and suspiciously sharp text.
//
// There are TWO because one density cannot serve both jobs. A proof trace wants
// columns — the 4×6 cell puts 160 of them across a 640-wide surface, and the
// trace reads unwrapped. Everything a player reads at a glance wants size — at
// that cell a capital is 1.4% of screen height, against PICO-8's 3.9%. So the
// cell is a per-call choice between two frozen sizes rather than a compromise
// that is wrong for both.
//
// Encoding: five octal digits per glyph, top row first, bit 4 = left column.
// Codepoints 32..95; lowercase folds to uppercase, anything else prints '?'.

const GLYPHS =
  '00000' + '22202' + '55000' + '57575' + '36363' + '51245' + '66757' + '22000' + // ' ' ! " # $ % & '
  '24442' + '21112' + '05250' + '02720' + '00024' + '00700' + '00002' + '11244' + // ( ) * + , - . /
  '75557' + '26227' + '71747' + '71317' + '55711' + '74717' + '74757' + '71111' + // 0-7
  '75757' + '75717' + '02020' + '02024' + '12421' + '07070' + '42124' + '71302' + // 8 9 : ; < = > ?
  '75747' + '25755' + '65656' + '34443' + '65556' + '74747' + '74744' + '34553' + // @ A-G
  '55755' + '72227' + '11152' + '55655' + '44447' + '57755' + '65555' + '25552' + // H-O
  '65644' + '25563' + '65655' + '34216' + '72222' + '55557' + '55552' + '55775' + // P-W
  '55255' + '55222' + '71247' + '32223' + '44211' + '62226' + '25000' + '00007';  // X-Z [ \ ] ^ _

/** Typographic punctuation an author will paste without thinking, folded to
 *  the ASCII the glyph table has. Silently printing '?' for an em dash is the
 *  kind of papercut that reads as a broken renderer. */
const FOLD = { '\u2014': '-', '\u2013': '-', '\u2018': "'", '\u2019': "'",
               '\u201c': '"', '\u201d': '"', '\u2026': '.', '\u00b7': '.',
               '\u00a0': ' ', '\u2192': '>', '\u2190': '<' };

/** Rows of a character, as [left, mid, right] booleans — 5 rows. */
export function glyph(ch) {
  let code = (FOLD[ch] ?? ch).charCodeAt(0);
  if (code >= 97 && code <= 122) code -= 32;          // fold lowercase
  if (code < 32 || code > 95) code = 63;              // '?'
  const off = (code - 32) * 5;
  const rows = [];
  for (let r = 0; r < 5; r++) {
    const bits = GLYPHS.charCodeAt(off + r) - 48;
    rows.push([(bits & 4) !== 0, (bits & 2) !== 0, (bits & 1) !== 0]);
  }
  return rows;
}

/** The LARGE glyph set: 5×7 letters in the 6×8 cell, as a sheet — sixteen
 *  glyphs across, four bands, ASCII 32..95 in order. Written as pixels rather
 *  than packed bits because a font is the one table where a typo is invisible
 *  in code and glaring on screen, and this way it is glaring in both. */
const BIG = [
  //   ! " #  $  %  &  '  (  )  *  +  ,  -  .  /
  '.......#...#.#..#.#...#..##..#.##....#.....#..#................................#',
  '.......#...#.#..#.#..######..##..#...#....#....#..#.#.#..#....................#.',
  '.......#.......######.#.....#.#.#........#......#..###...#....................#.',
  '.......#........#.#..###...#...#.........#......#.##########.....#####.......#..',
  '.......#.......#####..#.#.#...#.#.#......#......#..###...#....##............#...',
  '................#.#.####.#..###..#........#....#..#.#.#..#....#.........##..#...',
  '.......#........#.#...#..#..##.##.#........#..#..............#..........##.#....',
  // 0 1 2 3 4 5 6 7 8 9 : ; < = > ?
  '.###...#...###.#####...#.#####..##.#####.###..###..............#.......#....###.',
  '#...#.##..#...#...#...##.#.....#...#...##...##...#..##...##...#.........#..#...#',
  '#..##..#......#..#...#.#.####.#........##...##...#..##...##..#...#####...#.....#',
  '#.#.#..#.....#....#.#..#.....#####....#..###..####..........#.............#...#.',
  '##..#..#....#......######....##...#..#..#...#....#..##...##..#...#####...#...#..',
  '#...#..#...#...#...#...#.#...##...#..#..#...#...#...##...#....#.........#.......',
  '.###..###.#####.###....#..###..###...#...###..##........#......#.......#.....#..',
  // @ A B C D E F G H I J K L M N O
  '.###...#..####..###.###..##########.###.#...#.###....###...##....#...##...#.###.',
  '#...#.#.#.#...##...##..#.#....#....#...##...#..#......##..#.#....##.####..##...#',
  '#.####...##...##....#...##....#....#....#...#..#......##.#..#....#.#.##.#.##...#',
  '#.#.##...#####.#....#...#####.####.#.########..#......###...#....#.#.##..###...#',
  '#.#########...##....#...##....#....#...##...#..#..#...##.#..#....#...##...##...#',
  '#....#...##...##...##..#.#....#....#...##...#..#..#...##..#.#....#...##...##...#',
  '.###.#...#####..###.###..######.....#####...#.###..###.#...#######...##...#.###.',
  // P Q R S T U V W X Y Z [ \ ] ^ _
  '####..###.####..##########...##...##...##...##...######.###.#.....###...#.......',
  '#...##...##...##......#..#...##...##...##...##...#....#.#...#.......#..#.#......',
  '#...##...##...##......#..#...##...##...#.#.#..#.#....#..#....#......#.#...#.....',
  '####.#...#####..###...#..#...##...##.#.#..#....#....#...#.....#.....#...........',
  '#....#.#.##.#......#..#..#...##...##.#.#.#.#...#...#....#.....#.....#...........',
  '#....#..#.#..#.....#..#..#...#.#.#.##.###...#..#..#.....#......#....#...........',
  '#.....##.##...#####...#...###...#..#...##...#..#..#####.###.....#.###......#####',
];

/** Rows of a character in the LARGE cell — 7 rows of 5 booleans. */
export function glyphBig(ch) {
  let code = (FOLD[ch] ?? ch).charCodeAt(0);
  if (code >= 97 && code <= 122) code -= 32;
  if (code < 32 || code > 95) code = 63;
  const band = Math.floor((code - 32) / 16), col = ((code - 32) % 16) * 5;
  const rows = [];
  for (let r = 0; r < 7; r++) {
    const src = BIG[band * 7 + r];
    rows.push([0, 1, 2, 3, 4].map((k) => src[col + k] === '#'));
  }
  return rows;
}
