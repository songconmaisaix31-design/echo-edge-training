export default {
  rules: {
    // 基础正确性
    'color-no-invalid-hex': true,
    'property-no-unknown': true,
    'selector-pseudo-class-no-unknown': true,
    'declaration-no-important': true,
    // 去重与冗余。不启用 no-duplicate-selectors：主题按 shell → soft-blue → glass
    // 逐层覆盖，同一选择器在后层重新声明是既有约定，视觉由浏览器验收把关。
    'declaration-block-no-duplicate-properties': [
      true,
      {ignore: ['consecutive-duplicates-with-different-values']},
    ],
    'shorthand-property-no-redundant-values': true,
    'length-zero-no-unit': true,
    // 设计变量：重设计合并后启用命名与用法约束
    'custom-property-pattern': '^[a-z][a-z0-9]*(-[a-z0-9]+)*$',
    'custom-property-no-missing-var-function': true,
  },
}
