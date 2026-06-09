// TBX-P2: the pure Tester arg helpers, lifted out of the 781-line god-component
// so they're unit-testable + reusable in isolation (no React, no DOM). Behavior
// is identical to the previous in-component versions.
import type { ToolCallResult } from './mcp-client';

export interface SchemaProperty {
  type?: string | string[];
  description?: string;
  enum?: string[];
  items?: SchemaProperty;
}

export interface ToolInputSchema {
  type: string;
  properties?: Record<string, SchemaProperty>;
  required?: string[];
}

// Resolve {{N.field.path}} templates against prior step results.
// N is the step index (0-based). Path uses dot notation; numeric segments
// index arrays (e.g. {{0.assets.0.asset_path}}).
export function resolveTemplate(template: string, results: (ToolCallResult | undefined)[]): string {
  return template.replace(/\{\{(\d+)\.([^}]+)\}\}/g, (match, idxStr, path) => {
    const idx = parseInt(idxStr);
    const result = results[idx];
    if (!result || result.isError) return match;
    const text = result.content?.[0]?.text;
    if (!text) return match;
    try {
      let data: unknown = JSON.parse(text);
      for (const part of path.split('.')) {
        if (data === null || data === undefined) return match;
        if (Array.isArray(data)) {
          const i = parseInt(part);
          data = isNaN(i) ? undefined : data[i];
        } else if (typeof data === 'object') {
          data = (data as Record<string, unknown>)[part];
        } else {
          return match;
        }
      }
      return data === undefined || data === null ? match : String(data);
    } catch {
      return match;
    }
  });
}

export function coerceArgs(
  rawArgs: Record<string, string>,
  schema: ToolInputSchema,
  stepResults?: (ToolCallResult | undefined)[]
): Record<string, unknown> {
  const props = schema.properties ?? {};
  const out: Record<string, unknown> = {};
  for (const [k, v] of Object.entries(rawArgs)) {
    const resolved = stepResults ? resolveTemplate(v, stepResults) : v;
    if (resolved === '') continue;
    const prop = props[k];
    const t = Array.isArray(prop?.type) ? prop.type[0] : prop?.type;
    if (t === 'boolean') out[k] = resolved === 'true';
    else if (t === 'integer' || t === 'number') out[k] = Number(resolved);
    else if (t === 'array' || t === 'object') {
      // TBX-F2: array/object args are typed as JSON in the form — parse them so
      // tools like apply_ops (ops[]) are actually callable (was sent as a string).
      try { out[k] = JSON.parse(resolved); } catch { out[k] = resolved; }
    }
    else out[k] = resolved;
  }
  return out;
}

// TBX-F2: does `v` parse as JSON of the given schema type? (red-border signal)
export function isValidJsonOfType(v: string, t?: string): boolean {
  try {
    const p = JSON.parse(v);
    if (t === 'array') return Array.isArray(p);
    if (t === 'object') return typeof p === 'object' && p !== null && !Array.isArray(p);
    return true;
  } catch { return false; }
}

// TBX-F3: validate raw form args against the tool's input schema BEFORE sending —
// required fields present, numbers numeric, enums in range, array/object valid
// JSON. Returns human-readable errors (empty = OK).
export function validateArgs(
  rawArgs: Record<string, string>,
  schema: ToolInputSchema,
): string[] {
  const props = schema.properties ?? {};
  const errors: string[] = [];
  for (const req of schema.required ?? []) {
    if (!rawArgs[req] || rawArgs[req].trim() === '') errors.push(`"${req}" is required`);
  }
  for (const [k, v] of Object.entries(rawArgs)) {
    if (v == null || v.trim() === '' || v.includes('{{')) continue;  // skip empty / template refs
    const prop = props[k];
    const t = Array.isArray(prop?.type) ? prop!.type.filter(x => x !== 'null')[0] : prop?.type;
    if ((t === 'integer' || t === 'number') && Number.isNaN(Number(v))) errors.push(`"${k}" must be a number`);
    if (prop?.enum && !prop.enum.includes(v)) errors.push(`"${k}" must be one of: ${prop.enum.join(', ')}`);
    if ((t === 'array' || t === 'object') && !isValidJsonOfType(v, t)) errors.push(`"${k}" must be a valid JSON ${t}`);
  }
  return errors;
}
