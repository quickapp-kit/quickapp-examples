# Tabs 001

This baseline verifies a controlled Tabs component:

- `items="首页|任务|我的"` is a deterministic compact item representation.
- `selected="{{ selected }}"` is a `selected` BindingProperty owned by JS/Core state.
- `change` carries `{ index, value }` and the handler writes the selected state back.
- Conditional content proves state-driven rendering without a second tree.

Tabs is a removable Host Component. The package does not claim platform-native Tabs support.
