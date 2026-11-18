import assert from 'node:assert/strict'
import {test} from 'node:test'
import {fileURLToPath} from 'node:url'
import {ESLint} from 'eslint'
import stylelint from 'stylelint'

const frontend = fileURLToPath(new URL('../', import.meta.url))
const eslint = new ESLint({cwd: frontend})
const scriptFile = fileURLToPath(new URL('../src/lint-rule-case.tsx', import.meta.url))
const styleFile = fileURLToPath(new URL('../src/lint-rule-case.css', import.meta.url))

const invalidScripts = {
  '@typescript-eslint/no-explicit-any': 'export function identity(value: any) { return value }',
  '@typescript-eslint/consistent-type-assertions': 'export const count = 0 as number',
  '@typescript-eslint/ban-ts-comment': '// @ts-ignore\nexport const count = 0',
  'no-restricted-globals': 'export const response = fetch("/api/v1/me")',
  'no-restricted-properties': 'export const response = window.fetch("/api/v1/me")',
  'no-restricted-imports': 'import Page from "./Patients"; export default Page',
  'jsx-a11y/alt-text': 'export const Picture = () => <img src="example.svg" />',
  'react-hooks/rules-of-hooks': 'import {useState} from "react"; export function Panel({active}: {active: boolean}) { if (active) useState(0); return null }',
}

for (const [rule, code] of Object.entries(invalidScripts)) {
  test(rule, async () => {
    const [result] = await eslint.lintText(code, {filePath: scriptFile})
    assert.ok(result.messages.some(message => message.ruleId === rule && message.severity === 2))
  })
}

test('源码不能关闭网络边界', async () => {
  const code = `// eslint-disable-next-line no-restricted-globals\n${invalidScripts['no-restricted-globals']}`
  const [result] = await eslint.lintText(code, {filePath: scriptFile})
  assert.ok(result.messages.some(message => message.ruleId === 'no-restricted-globals' && message.severity === 2))
})

const invalidStyles = {
  'color-no-invalid-hex': '.text { color: #xyz; }',
  'declaration-no-important': '.text { color: red !important; }',
}

for (const [rule, code] of Object.entries(invalidStyles)) {
  test(rule, async () => {
    const result = await stylelint.lint({code, codeFilename: styleFile})
    assert.ok(result.errored)
    assert.ok(result.results.some(file => file.warnings.some(message => message.rule === rule)))
  })
}
