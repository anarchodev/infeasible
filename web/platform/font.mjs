// font.mjs — the built-in 3×5 glyph set, drawn in the frozen 4×6 text cell.
//
// §12 freezes text METRICS, not letter shapes: `print` advances GLYPH_W per
// character and a line is GLYPH_H tall on every backend, so a native player may
// substitute its own font without any cart's UI shifting by a pixel. This is
// the Canvas2D backend's font, and it is bitmap rather than `fillText` for one
// reason: text renders at internal resolution like everything else, or the
// aesthetic splits into game pixels and suspiciously sharp text.
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

/** Rows of a character, as [left, mid, right] booleans — 5 rows. */
export function glyph(ch) {
  let code = ch.charCodeAt(0);
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
