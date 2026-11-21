import assert from 'node:assert/strict'
import {execFileSync} from 'node:child_process'
import {readFile} from 'node:fs/promises'
import {fileURLToPath, pathToFileURL} from 'node:url'
import {compile} from 'json-schema-to-typescript'

const backend = fileURLToPath(new URL('../../backend/', import.meta.url))
const destination = new URL('../src/analysis-api.generated.ts', import.meta.url)
const multiviewDestination = new URL('../src/multiview-api.generated.ts', import.meta.url)
const json = 'application/json'
const media = '/api/v1/media/{identifier}'

function checkResponses(document, schema) {
  const operations = [
    [media + '/analysis-jobs', 'get', 200],
    [media + '/analysis-jobs', 'post', 201],
    [media + '/structure-jobs', 'post', 201],
    [media + '/review-jobs', 'post', 201],
    ['/api/v1/examinations/{key}/analysis-jobs', 'get', 200],
  ]
  for (const [path, method, status] of operations) {
    const response = document.paths[path][method].responses[status].content[json].schema
    const job = method === 'get' ? response.items : response
    assert.deepEqual(job.oneOf, schema.oneOf, `分析任务的响应类型不一致 ${method} ${path}`)
    assert.deepEqual(job.discriminator, schema.discriminator, `分析状态的判别字段不一致 ${method} ${path}`)
  }
}

async function generate(python) {
  const env = Object.fromEntries(Object.entries(process.env).filter(([name]) => !/^ECHO_|^PYTHONPATH$/i.test(name)))
  const document = JSON.parse(execFileSync(python, ['-X', 'utf8', '-c', 'import json; from main import app; print(json.dumps(app.openapi()))'], {
    cwd: backend, env: {...env, ECHO_DATABASE_URL: 'sqlite://'}, encoding: 'utf8', windowsHide: true,
  }))
  const schema = document.paths['/api/v1/analysis-jobs/{key}'].get.responses['200'].content[json].schema
  checkResponses(document, schema)
  const components = document.components.schemas
  const schemas = Object.fromEntries(['SucceededAnalysisJob', 'InsufficientEvidenceAnalysisJob', 'OtherAnalysisJob',
    'InsufficientEvidence'].map(name => [name, components[name]]))
  const options = {bannerComment: '', style: {singleQuote: true, semi: false}, unknownAny: true}
  const jobs = await compile({...schema, title: 'AnalysisJobResponse', components: {schemas}}, 'AnalysisJobResponse', options)
  const structureRequest = document.paths[media + '/structure-jobs'].post.requestBody.content[json].schema
  assert.equal(structureRequest.$ref, '#/components/schemas/StructureInput', '四腔分割请求类型已更改')
  const structure = await compile(components.StructureInput, 'StructureInput', options)
  const multiview = document.paths['/api/v1/examinations/{key}/multiview']
  const assessment = multiview.get.responses[200].content[json].schema
  const saved = multiview.put.responses[200].content[json].schema
  assert.deepEqual(assessment.oneOf, saved.oneOf, '多切面读取和保存的响应类型不一致')
  assert.deepEqual(assessment.discriminator, saved.discriminator, '多切面读取和保存的判别字段不一致')
  const assessmentSchemas = Object.fromEntries([
    'AssessmentInsufficient', 'AssessmentSucceeded', 'Coverage', 'Finding', 'FrameEvidence', 'InsufficientEvidence',
  ].map(name => [name, components[name]]))
  const assessmentTypes = await compile({...assessment, title: 'AssessmentResponse', components: {schemas: assessmentSchemas}},
    'AssessmentResponse', options)
  const banner = '// 由 FastAPI OpenAPI 生成。\n\n'
  return {analysis: banner + jobs + '\n' + structure, assessment: banner + assessmentTypes}
}

export async function checkAnalysisContract(python) {
  const generated = await generate(python)
  assert.ok((await readFile(destination, 'utf8')).replaceAll('\r\n', '\n') === generated.analysis,
    '分析任务的 TypeScript 类型与 OpenAPI 不一致。运行 npm --prefix frontend run generate:analysis-api 查看新定义。')
  assert.ok((await readFile(multiviewDestination, 'utf8')).replaceAll('\r\n', '\n') === generated.assessment,
    '多切面评估的 TypeScript 类型与 OpenAPI 不一致。运行 npm --prefix frontend run generate:analysis-api -- multiview 查看新定义。')
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  const python = process.env.ECHO_VERIFY_PYTHON || (process.platform === 'win32' ? 'python' : 'python3')
  if (process.argv[2] === '--print') {
    const generated = await generate(python)
    process.stdout.write(process.argv[3] === 'multiview' ? generated.assessment : generated.analysis)
  }
  else await checkAnalysisContract(python)
}
