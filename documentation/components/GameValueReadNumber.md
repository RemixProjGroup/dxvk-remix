# Game Value Read Number

Reads a plugin\-injected game\-state value as a float\.<br/><br/>Plugins write values via remixapi\_SetGameValue\(key, value\) in remix\_c\.h\. This component reads the value under the given key and parses it as a float\. Accepts any string parseable as a C floating\-point literal; otherwise the default is emitted\.

## Component Information

- **Name:** `GameValueReadNumber`
- **UI Name:** Game Value Read Number
- **Version:** 1
- **Categories:** Sense

## Input Properties

| Property | Display Name | Type | IO Type | Default Value | Optional |
|----------|--------------|------|---------|---------------|----------|
| key | Key | String | Input | "" | No | 
| defaultValue | Default | Float | Input | 0\.0 | No | 

### Key

The game\-state key to read \(e\.g\. 'windSpeed'\)\. Plugins publish values under this key via remixapi\_SetGameValue in remix\_c\.h\.


### Default

Value emitted when the key has not been set by any plugin, or when the stored value does not parse as a number\.


## Output Properties

| Property | Display Name | Type | IO Type | Default Value | Optional |
|----------|--------------|------|---------|---------------|----------|
| value | Value | Float | Output | 0\.0 | No | 

### Value

The current value of the game\-state key as a float\. Accepts any string parseable as a C floating\-point literal \(trailing whitespace permitted\); otherwise the default is emitted\.


## Usage Notes

This component is part of the RTX Remix graph system. It is intended for use in the Remix Toolkit and Runtime only.

---
[← Back to Component Index](index.md)
