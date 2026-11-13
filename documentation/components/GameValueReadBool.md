# Game Value Read Bool

Reads a plugin\-injected game\-state value as a bool\.<br/><br/>Plugins write values via remixapi\_SetGameValue\(key, value\) in remix\_c\.h\. This component reads the value under the given key and interprets it as a bool\. Accepts '1'/'0'/'true'/'false' \(case\-insensitive\); any other content emits the default\.

## Component Information

- **Name:** `GameValueReadBool`
- **UI Name:** Game Value Read Bool
- **Version:** 1
- **Categories:** Sense

## Input Properties

| Property | Display Name | Type | IO Type | Default Value | Optional |
|----------|--------------|------|---------|---------------|----------|
| key | Key | String | Input | "" | No | 
| defaultValue | Default | Bool | Input | false | No | 

### Key

The game\-state key to read \(e\.g\. 'isRaining'\)\. Plugins publish values under this key via remixapi\_SetGameValue in remix\_c\.h\.


### Default

Value emitted when the key has not been set by any plugin, or when the stored value does not parse as a bool\.


## Output Properties

| Property | Display Name | Type | IO Type | Default Value | Optional |
|----------|--------------|------|---------|---------------|----------|
| value | Value | Bool | Output | false | No | 

### Value

The current value of the game\-state key as a bool\. Accepts '1'/'0'/'true'/'false' \(case\-insensitive\); any other content yields the default\.


## Usage Notes

This component is part of the RTX Remix graph system. It is intended for use in the Remix Toolkit and Runtime only.

---
[← Back to Component Index](index.md)
