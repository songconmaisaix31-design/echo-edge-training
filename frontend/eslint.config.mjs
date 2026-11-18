import jsxA11y from 'eslint-plugin-jsx-a11y'
import hooks from 'eslint-plugin-react-hooks'
import tseslint from 'typescript-eslint'

const pageModules = ['Modules', 'Exams', 'Patients', 'Diagnosis']

export default tseslint.config(
  {
    files: ['src/**/*.{ts,tsx}'],
    extends: [tseslint.configs.strict],
    linterOptions: {reportUnusedDisableDirectives: 'error', noInlineConfig: true},
    rules: {
      '@typescript-eslint/no-explicit-any': 'error',
      '@typescript-eslint/ban-ts-comment': ['error', {
        'ts-expect-error': true, 'ts-ignore': true, 'ts-nocheck': true,
      }],
    },
  },
  {
    files: ['src/**/*.tsx'],
    plugins: {'jsx-a11y': jsxA11y, 'react-hooks': hooks},
    rules: {
      ...jsxA11y.configs.recommended.rules,
      'react-hooks/rules-of-hooks': 'error',
    },
  },
  {
    files: ['src/**/*.{ts,tsx}'],
    ignores: ['src/api.ts'],
    rules: {
      '@typescript-eslint/consistent-type-assertions': ['error', {assertionStyle: 'never'}],
      'no-restricted-globals': ['error', ...['fetch', 'XMLHttpRequest', 'WebSocket', 'EventSource'].map(name => ({
        name, message: '网络请求通过 api.ts。',
      }))],
      'no-restricted-properties': ['error', ...['window', 'globalThis', 'self'].map(object => ({
        object, property: 'fetch', message: '网络请求通过 api.ts。',
      }))],
    },
  },
  {
    files: ['src/**/*.{ts,tsx}'],
    ignores: ['src/main.tsx'],
    rules: {
      'no-restricted-imports': ['error', {
        patterns: [{
          group: pageModules.flatMap(name => [`**/${name}`, `**/${name}.tsx`]),
          message: '页面由入口组合。共享组件和契约放在独立模块。',
        }],
      }],
    },
  },
)
