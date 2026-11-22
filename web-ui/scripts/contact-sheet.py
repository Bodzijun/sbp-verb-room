"""Create local QA contact sheets from the captured screenshots (Pillow required)."""
from pathlib import Path
from PIL import Image, ImageDraw
root = Path(__file__).resolve().parents[1] / 'shots'
for size in ('1280x800', '900x600'):
    paths = sorted(p for p in root.glob(f'*-{size}.png') if not p.name.startswith(('compare-', 'contact-', 'layout-choice-')))
    sheet = Image.new('RGB', (1600, ((len(paths) + 3) // 4) * 282), '#101518')
    draw = ImageDraw.Draw(sheet)
    for i, path in enumerate(paths):
        image = Image.open(path).convert('RGB')
        image.thumbnail((396, 254))
        x, y = (i % 4) * 400, (i // 4) * 282
        sheet.paste(image, (x, y + 22))
        draw.text((x + 3, y + 4), path.stem, fill='#d4e5ec')
    sheet.save(root / f'contact-{size}.png')
paths = sorted(p for p in (root.parents[1] / 'ui-mockup' / 'shots').glob('*.png') if not p.name.startswith('before-'))
sheet = Image.new('RGB', (1600, ((len(paths) + 3) // 4) * 282), '#101518')
draw = ImageDraw.Draw(sheet)
for i, path in enumerate(paths):
    image = Image.open(path).convert('RGB')
    image.thumbnail((396, 254))
    x, y = (i % 4) * 400, (i // 4) * 282
    sheet.paste(image, (x, y + 22))
    draw.text((x + 3, y + 4), f'v6 reference / {path.stem}', fill='#d4e5ec')
sheet.save(root / 'reference-v6-contact.png')

# Required evidence: full-size left/reference | right/implementation for EVERY PNG,
# including historical before-mix/output states (mapped to the functional tabs).
reference = root.parents[1] / 'ui-mockup' / 'shots'
for ref in sorted(reference.glob('*.png')):
    for size in ('1280x800', '900x600'):
        current = root / f'{ref.stem}-{size}.png'
        if not current.exists():
            raise RuntimeError(f'Missing screenshot for {ref.name}: {current}')
        w, h = map(int, size.split('x'))
        sheet = Image.new('RGB', (2*w, h+32), '#101518')
        draw = ImageDraw.Draw(sheet)
        draw.text((12, 8), f'v6 reference / {ref.stem} (original proportions)', fill='#c6dfd5')
        draw.text((w+12, 8), f'T5 / {ref.stem} / layout B / {size}', fill='#c6dfd5')
        left = Image.open(ref).convert('RGB'); left.thumbnail((w,h))
        sheet.paste(left,(0,32)); sheet.paste(Image.open(current).convert('RGB'),(w,32))
        sheet.save(root / f'compare-{ref.stem}{"" if size == "1280x800" else "-900x600"}.png')
# Layout A was removed in T5; layout-choice sheets are no longer produced.
