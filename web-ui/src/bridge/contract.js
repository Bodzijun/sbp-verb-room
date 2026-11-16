// Offline validator for the JSON Schema vocabulary used by the frozen v1 schemas.
// This is intentionally not a general-purpose Draft 2020-12 implementation.
export const clone = value => JSON.parse(JSON.stringify(value));
export const SCHEMAS = ['common', 'parameters', 'capabilities', 'state', 'command', 'event', 'envelope', 'fixture', 'fixture-parameters'];
export async function loadContract(read = async name => {
  const response = await fetch(new URL(`../../schema/${name}.schema.json`, import.meta.url));
  if (!response.ok) throw new Error(`Cannot load local schema ${name}`);
  return response.json();
}) {
  const schemas = await Promise.all(SCHEMAS.map(read));
  return createValidator(schemas);
}
export function createValidator(schemas) {
  const registry = new Map(schemas.map(s => [s.$id, s]));
  function check(value, schema, root, path = '$') {
    const errors = [];
    const fail = reason => errors.push(`${path}: ${reason}`);
    if (typeof schema === 'boolean') return schema ? [] : [`${path}: forbidden`];
    if (schema.$ref) {
      const [id, fragment = ''] = schema.$ref.split('#');
      const doc = id ? registry.get(id) : root;
      if (!doc) throw new Error(`Unregistered local schema ${id}`);
      const ref = fragment.split('/').slice(1).reduce((v, key) => v[key.replace(/~1/g, '/').replace(/~0/g, '~')], doc);
      errors.push(...check(value, ref, doc, path));
    }
    if (schema.const !== undefined && JSON.stringify(value) !== JSON.stringify(schema.const)) fail('const');
    if (schema.enum && !schema.enum.some(v => JSON.stringify(v) === JSON.stringify(value))) fail('enum');
    const types = { object: v => v !== null && typeof v === 'object' && !Array.isArray(v), array: Array.isArray,
      number: v => typeof v === 'number' && Number.isFinite(v), integer: Number.isInteger,
      string: v => typeof v === 'string', boolean: v => typeof v === 'boolean', null: v => v === null };
    if (schema.type && ![schema.type].flat().some(t => types[t](value))) return [...errors, `${path}: type ${schema.type}`];
    for (const item of schema.allOf || []) errors.push(...check(value, item, root, path));
    for (const key of ['anyOf', 'oneOf']) if (schema[key]) {
      const matches = schema[key].filter(s => check(value, s, root, path).length === 0).length;
      if (key === 'anyOf' ? matches === 0 : matches !== 1) fail(key);
    }
    if (schema.if) {
      const branch = check(value, schema.if, root, path).length ? schema.else : schema.then;
      if (branch) errors.push(...check(value, branch, root, path));
    }
    if (typeof value === 'number') {
      if (!Number.isFinite(value)) fail('finite number required');
      if (schema.minimum !== undefined && value < schema.minimum) fail('minimum');
      if (schema.maximum !== undefined && value > schema.maximum) fail('maximum');
      if (schema.exclusiveMinimum !== undefined && value <= schema.exclusiveMinimum) fail('exclusiveMinimum');
      if (schema.exclusiveMaximum !== undefined && value >= schema.exclusiveMaximum) fail('exclusiveMaximum');
    }
    if (typeof value === 'string') {
      if (schema.minLength !== undefined && [...value].length < schema.minLength) fail('minLength');
      if (schema.maxLength !== undefined && [...value].length > schema.maxLength) fail('maxLength');
      if (schema.pattern && !new RegExp(schema.pattern).test(value)) fail('pattern');
    }
    if (Array.isArray(value)) {
      if (schema.minItems !== undefined && value.length < schema.minItems) fail('minItems');
      if (schema.maxItems !== undefined && value.length > schema.maxItems) fail('maxItems');
      if (schema.items) value.forEach((v, i) => errors.push(...check(v, schema.items, root, `${path}[${i}]`)));
    } else if (value !== null && typeof value === 'object') {
      const keys = Object.keys(value), properties = schema.properties || {}, patterns = schema.patternProperties || {};
      for (const key of schema.required || []) if (!(key in value)) fail(`required ${key}`);
      if (schema.minProperties !== undefined && keys.length < schema.minProperties) fail('minProperties');
      if (schema.maxProperties !== undefined && keys.length > schema.maxProperties) fail('maxProperties');
      for (const key of keys) {
        if (properties[key]) errors.push(...check(value[key], properties[key], root, `${path}.${key}`));
        const matching = Object.keys(patterns).filter(p => new RegExp(p).test(key));
        matching.forEach(p => errors.push(...check(value[key], patterns[p], root, `${path}.${key}`)));
        if (!(key in properties) && !matching.length && schema.additionalProperties === false) fail(`unexpected ${key}`);
      }
    }
    return errors;
  }
  return {
    errors(value, name = 'envelope', def) {
      const root = registry.get(`https://verbroom.invalid/web-ui/schema/${name}.schema.json`);
      return check(value, def ? root.$defs[def] : root, root);
    },
    assert(value, name = 'envelope', def) {
      const errors = this.errors(value, name, def);
      if (errors.length) throw new Error(`Contract validation: ${errors.slice(0, 8).join('; ')}`);
      return value;
    }
  };
}
