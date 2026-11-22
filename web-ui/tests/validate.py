"""Independent full Draft 2020-12 verification; never resolves network references."""
import json
from pathlib import Path
import jsonschema
from referencing import Registry, Resource

root = Path(__file__).resolve().parents[1]
schemas = [json.loads(p.read_text(encoding='utf-8-sig')) for p in (root / 'schema').glob('*.json')]
registry = Registry().with_resources((s['$id'], Resource.from_contents(s)) for s in schemas)
by_name = {s['$id'].split('/')[-1]: s for s in schemas}
for schema in schemas:
    jsonschema.Draft202012Validator.check_schema(schema)
count = 0
for path in (root / 'fixtures').glob('*.json'):
    name = 'fixture-parameters.schema.json' if path.name.startswith('parameters') else 'fixture.schema.json'
    jsonschema.Draft202012Validator(by_name[name], registry=registry).validate(json.loads(path.read_text(encoding='utf-8-sig')))
    count += 1
audit_path = root / 'tests' / 'browser-audit.json'
messages = 0
if audit_path.exists():
    validator = jsonschema.Draft202012Validator(by_name['envelope.schema.json'], registry=registry)
    for entry in json.loads(audit_path.read_text(encoding='utf-8')):
        if entry['direction'] in ('in', 'out'):
            validator.validate(entry['message'])
            messages += 1
print(f'PASS: {len(schemas)} schemas, {count} frozen fixtures, {messages} captured browser messages (Draft 2020-12, offline)')
