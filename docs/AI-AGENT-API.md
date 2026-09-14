# 【20260914】（对外）C端智能体业务开放能力-设备侧接口

> **面向对象**：涂鸦对外开放设备侧接口能力，供开发者使用。

---

## 1. 业务说明

### 1.1 角色

角色分为**自定义角色**和**模板角色**两种：

- **自定义角色**：可以由用户创建或修改。
- **模板角色**：只能使用，不能修改。IoT 平台上智能体配置的是模板角色，变更后 C 端的模板角色会同步更新。

### 1.2 聊天记录和记忆

聊天记录和记忆归属于角色下。

---

## 2. 智能体配置服务

| 接口地址 | 描述 |
|---------|------|
| `thing.ai.agent.config.device.detail` | 获取设备详情 |
| `thing.ai.agent.config.languages.list` | 获取支持的语言列表 |

---

### 2.1 获取设备详情

**接口地址：** `thing.ai.agent.config.device.detail`

#### 请求参数

无。

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码（详情见错误码章节），成功时为空 |
| success | Boolean | 是否成功（`true`：成功，`false`：失败） |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Object | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| id | String | 设备 ID |
| uuid | String | 设备 UUID |
| productId | String | 产品 ID |
| name | String | 设备名称 |
| customName | String | 自定义设备名称（当有值时优先取自定义名称） |
| iconUrl | String | 设备图标访问地址（以 http(s) 开头） |
| iconUrlResizable | String | 设备图标访问地址（以 http(s) 开头）（可裁切） |
| icon | String | 设备图标 |
| langCode | String | 设备配网时的语种编码 |
| langName | String | 设备配网时的语种名称 |
| homeCity | String | 当前设备所在家庭城市 |

#### 请求示例

```
thing.ai.agent.config.device.detail
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": {
    "iconUrlResizable": "String",
    "productId": "String",
    "name": "String",
    "icon": "String",
    "customName": "String",
    "id": "String",
    "iconUrl": "String",
    "uuid": "String",
    "langCode": "String",
    "langName": "String",
    "homeCity": "String"
  }
}
```

---

### 2.2 获取支持的语言列表

**接口地址：** `thing.ai.agent.config.languages.list`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码（详情见错误码章节），成功时为空 |
| success | Boolean | 是否成功（`true`：成功，`false`：失败） |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Array | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| langCode | String | 语言 code |
| langName | String | 语言名称 |
| hasDefault | boolean | 是否默认语言 |

#### 请求示例

```
thing.ai.agent.config.languages.list
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": [
    {
      "hasDefault": "boolean",
      "langCode": "String",
      "langName": "String"
    }
  ]
}
```

#### 错误码

| 错误码 | 说明 |
|--------|------|
| 500 | 系统错误 |

---

## 3. 智能体角色服务

| 接口地址 | 描述 |
|---------|------|
| `thing.ai.agent.role.initialize-agent-role-binding` | 初始化角色和智能体的绑定关系 |
| `thing.ai.agent.role.role-template.list` | 查询 AI 智能体角色模板列表 |
| `thing.ai.agent.role.custom-role.page` | 查询 AI 智能体自定义角色列表（分页） |
| `thing.ai.agent.role.custom-role.update` | 修改 AI 智能体自定义角色 |
| `thing.ai.agent.role.custom-role.delete` | 删除 AI 智能体自定义角色 |
| `thing.ai.agent.role.bind-with-role` | AI 智能体绑定角色 |
| `thing.ai.agent.role.get-bind-role` | 查询智能体绑定的智能体角色 |
| `thing.ai.agent.role.update-workflow` | 更新智能体角色的工作流类型 |

> ⚠️ `thing.ai.agent.role.update-workflow` 在 API 列表中提及，本页暂无详细接口文档。

---

### 3.1 初始化角色和智能体的绑定关系

**接口地址：** `thing.ai.agent.role.initialize-agent-role-binding`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Object | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| roleId | String | 角色 ID |
| roleName | String | 角色名称 |
| roleDesc | String | 角色描述 |
| roleIntroduce | String | 角色介绍 |
| roleImgUrl | String | 角色图像 |
| roleImgUrlResizable | String | 角色图像（可裁切） |
| useLangCode | String | 角色使用语言 code |
| useLangName | String | 角色使用语言名称 |
| useTimbreId | String | 角色使用音色 |
| isUserCloneTimbre | boolean | 是否是自定义克隆音色 |
| useTimbreName | String | 角色使用音色名称 |
| useTimbreSupportLangs | String | 角色支持的音色语言，多个用逗号分隔 |
| useTimbreTags | String[] | 角色标签 |
| useLlmId | String | 角色使用语言模型 |
| useLlmName | String | 角色使用语言模型名称 |
| memoryInfo | String | 记忆体 |
| speed | double | 音色速度 |
| tone | double | 音色语气 |
| templateId | String | 模板 ID |
| bindRoleType | int | 绑定角色类型 |
| lastTextAnswer | String | 最近的一次回复文本 |
| llmSupportFuncTags | String | 语言模型支持的函数标签，多个用逗号分隔 |
| roleVariables | Object | 角色变量 |

#### 请求示例

```
thing.ai.agent.role.initialize-agent-role-binding
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": {
    "memoryInfo": "String",
    "bindRoleType": "int",
    "isUserCloneTimbre": "boolean",
    "tone": "double",
    "roleId": "String",
    "llmSupportFuncTags": "String",
    "roleIntroduce": "String",
    "useTimbreTags": ["String[]"],
    "templateId": "String",
    "lastTextAnswer": "String",
    "speed": "double",
    "useTimbreSupportLangs": "String",
    "useLlmId": "String",
    "useTimbreId": "String",
    "roleDesc": "String",
    "roleImgUrl": "String",
    "roleImgUrlResizable": "String",
    "roleName": "String",
    "useLangCode": "String",
    "useTimbreName": "String",
    "roleVariables": {},
    "useLangName": "String",
    "useLlmName": "String"
  }
}
```

---

### 3.2 查询 AI 智能体角色模板列表

**接口地址：** `thing.ai.agent.role.role-template.list`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| tagCode | String | body | false | 标签 |
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Array | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| templateId | String | 角色模板 ID |
| roleId | String | 角色 ID |
| roleCode | String | 角色编码 |
| roleName | String | 角色名称 |
| roleDesc | String | 角色介绍 |
| roleImgUrl | String | 角色图标 |
| roleImgUrlResizable | String | 角色图像（可裁切） |
| roleIntroduce | String | 角色特征 |
| useLangCode | String | 角色使用语言 |
| useLangName | String | 角色使用语言 |
| useTimbreId | String | 角色使用音色 |
| useTimbreName | String | 角色使用音色名称 |
| useTimbreSupportLangs | String | 角色支持的音色语言，多个用逗号分隔 |
| useTimbreSupportLangNames | String | 角色支持的音色语言名称，多个用逗号分隔 |
| useLlmId | String | 角色使用语言模型 |
| useLlmName | String | 角色使用语言模型名称 |
| defaultFlag | int | 默认标识（1 代表是默认） |
| lastTextAnswer | String | 最近的一次回复文本 |

#### 请求示例

```
thing.ai.agent.role.role-template.list
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": [
    {
      "defaultFlag": "int",
      "roleId": "String",
      "roleIntroduce": "String",
      "templateId": "String",
      "lastTextAnswer": "String",
      "useTimbreSupportLangs": "String",
      "useLlmId": "String",
      "useTimbreId": "String",
      "roleDesc": "String",
      "roleImgUrl": "String",
      "roleCode": "String",
      "roleImgUrlResizable": "String",
      "roleName": "String",
      "useLangCode": "String",
      "useTimbreName": "String",
      "useTimbreSupportLangNames": "String",
      "useLangName": "String",
      "useLlmName": "String"
    }
  ]
}
```

---

### 3.3 查询 AI 智能体自定义角色列表（分页）

**接口地址：** `thing.ai.agent.role.custom-role.page`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| pageNo | int | body | true | 页码 |
| pageSize | int | body | true | 每页条数 |
| roleCategory | String | body | false | 角色分类 |
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Array | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| roleId | String | 角色 ID |
| roleName | String | 角色名称 |
| roleDesc | String | 角色描述 |
| roleIntroduce | String | 角色介绍 |
| roleImgUrl | String | 角色图像 |
| roleImgUrlResizable | String | 角色图像（可裁切） |
| useLangCode | String | 角色使用语言 code |
| useLangName | String | 角色使用语言名称 |
| useTimbreId | String | 角色使用音色 |
| useTimbreName | String | 角色使用音色名称 |
| useLlmId | String | 角色使用语言模型 |
| useLlmName | String | 角色使用语言模型名称 |
| templateId | String | 模板 ID |
| lastTextAnswer | String | 最近的一次回复文本 |

#### 请求示例

```
thing.ai.agent.role.custom-role.page
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": [
    {
      "roleId": "String",
      "roleIntroduce": "String",
      "templateId": "String",
      "lastTextAnswer": "String",
      "useLlmId": "String",
      "useTimbreId": "String",
      "roleDesc": "String",
      "roleImgUrl": "String",
      "roleImgUrlResizable": "String",
      "roleName": "String",
      "useLangCode": "String",
      "useTimbreName": "String",
      "useLangName": "String",
      "useLlmName": "String"
    }
  ]
}
```

---

### 3.4 修改 AI 智能体自定义角色

**接口地址：** `thing.ai.agent.role.custom-role.update`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| roleId | String | body | true | 角色 ID |
| roleName | String | body | false | 角色名称 |
| roleDesc | String | body | false | 角色描述 |
| roleIntroduce | String | body | false | 角色介绍 |
| roleImgUrl | String | body | false | 角色图像 |
| useLangCode | String | body | false | 角色使用语言 |
| useTimbreId | String | body | false | 角色使用音色 |
| useLlmId | String | body | false | 角色使用语言模型 |
| speed | String | body | false | 音色速度 |
| needBind | boolean | body | false | 是否需要绑定 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.role.custom-role.update
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": true
}
```

---

### 3.5 删除 AI 智能体自定义角色

**接口地址：** `thing.ai.agent.role.custom-role.delete`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| roleId | String | body | true | 角色 ID |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.role.custom-role.delete
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": false
}
```

---

### 3.6 AI 智能体绑定角色

**接口地址：** `thing.ai.agent.role.bind-with-role`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| bindRoleType | int | body | true | 0-自定义智能体角色，1-智能体角色模板，2-单角色场景默认角色 |
| roleId | String | body | true | 角色 ID（会根据 bindRoleType 决定具体含义） |
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.role.bind-with-role
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": false
}
```

---

### 3.7 查询智能体绑定的智能体角色

**接口地址：** `thing.ai.agent.role.get-bind-role`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Object | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| roleId | String | 角色 ID |
| roleName | String | 角色名称 |
| roleDesc | String | 角色描述 |
| roleIntroduce | String | 角色介绍 |
| roleImgUrl | String | 角色图像 |
| roleImgUrlResizable | String | 角色图像（可裁切） |
| useLangCode | String | 角色使用语言 code |
| useLangName | String | 角色使用语言名称 |
| useTimbreId | String | 角色使用音色 |
| isUserCloneTimbre | boolean | 是否是自定义克隆音色 |
| useTimbreName | String | 角色使用音色名称 |
| useTimbreSupportLangs | String | 角色支持的音色语言，多个用逗号分隔 |
| useTimbreTags | String[] | 角色标签 |
| useLlmId | String | 角色使用语言模型 |
| useLlmName | String | 角色使用语言模型名称 |
| memoryInfo | String | 记忆体 |
| speed | double | 音色速度 |
| tone | double | 音色语气 |
| templateId | String | 模板 ID |
| bindRoleType | int | 绑定角色类型 |
| lastTextAnswer | String | 最近的一次回复文本 |
| llmSupportFuncTags | String | 语言模型支持的函数标签，多个用逗号分隔 |
| roleVariables | Object | 角色变量 |

#### 请求示例

```
thing.ai.agent.role.get-bind-role
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": {
    "memoryInfo": "String",
    "bindRoleType": "int",
    "isUserCloneTimbre": "boolean",
    "tone": "double",
    "roleId": "String",
    "llmSupportFuncTags": "String",
    "roleIntroduce": "String",
    "useTimbreTags": ["String[]"],
    "templateId": "String",
    "lastTextAnswer": "String",
    "speed": "double",
    "useTimbreSupportLangs": "String",
    "useLlmId": "String",
    "useTimbreId": "String",
    "roleDesc": "String",
    "roleImgUrl": "String",
    "roleImgUrlResizable": "String",
    "roleName": "String",
    "useLangCode": "String",
    "useTimbreName": "String",
    "roleVariables": {},
    "useLangName": "String",
    "useLlmName": "String"
  }
}
```

#### 错误码

| 错误码 | 说明 |
|--------|------|
| 500 | 系统错误 |

---

## 4. 智能体角色聊天服务

| 接口地址 | 描述 |
|---------|------|
| `thing.ai.agent.chat.history.delete` | 删除 AI 智能体角色的历史会话记录 |
| `thing.ai.agent.chat.memory.delete` | 删除 AI 智能体角色的记忆 |
| `thing.ai.agent.chat.context.clear` | 清除 AI 智能体角色的上下文 |

---

### 4.1 删除 AI 智能体角色的历史会话记录

**接口地址：** `thing.ai.agent.chat.history.delete`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| bindRoleType | int | body | true | 0-自定义智能体角色，1-智能体角色模板，2-单角色场景默认角色 |
| roleId | String | body | true | 角色 ID（会根据 bindRoleType 决定具体含义） |
| clearAllHistory | boolean | body | true | 是否清除全部历史记录 |
| requestIds | String | body | false | 待删除历史记录的 requestId（多个用逗号隔开） |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.chat.history.delete
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": false
}
```

---

### 4.2 删除 AI 智能体角色的记忆

**接口地址：** `thing.ai.agent.chat.memory.delete`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| bindRoleType | int | body | true | 0-自定义智能体角色，1-智能体角色模板，2-单角色场景默认角色 |
| roleId | String | body | true | 角色 ID（会根据 bindRoleType 决定具体含义） |
| clearAllMemory | boolean | body | true | 是否清除全部记忆 |
| memoryKeys | String | body | false | 待删除记忆的 memoryKey（多个用逗号隔开） |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.chat.memory.delete
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": false
}
```

---

### 4.3 清除 AI 智能体角色的上下文

**接口地址：** `thing.ai.agent.chat.context.clear`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| bindRoleType | int | body | true | 0-自定义智能体角色，1-智能体角色模板，2-单角色场景默认角色 |
| roleId | String | body | true | 角色 ID（会根据 bindRoleType 决定具体含义） |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | boolean | 返回结果 |

#### 请求示例

```
thing.ai.agent.chat.context.clear
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": false
}
```

#### 错误码

| 错误码 | 说明 |
|--------|------|
| 500 | 系统错误 |

---

## 5. 音色服务

| 接口地址 | 描述 |
|---------|------|
| `thing.ai.timbre.market.page` | 查询官方音色（分页） |

---

### 5.1 查询官方音色（分页）

**接口地址：** `thing.ai.timbre.market.page`

#### 请求参数

| 参数名 | 类型 | 参数位置 | 必需 | 描述 |
|--------|------|----------|------|------|
| pageNo | int | body | false | 页码 |
| pageSize | int | body | false | 每页条数 |
| lang | String | body | false | 音色支持的语种 |
| keyWord | String | body | false | 关键字 |
| tag | String | body | false | 音色标签 |
| requestLang | String | body | false | 请求的语种 |

#### 返回参数

| 参数名 | 类型 | 说明 |
|--------|------|------|
| code | Integer | 响应码，成功时为空 |
| success | Boolean | 是否成功 |
| msg | String | 请求失败的信息，成功时为空 |
| t | Long | 返回时间戳，13 位 |
| result | Array | 返回结果 |

#### result 参数说明

| 参数名 | 类型 | 描述 |
|--------|------|------|
| voiceId | String | 语音标识符 |
| voiceName | String | 语音名称 |
| descTags | String[] | 描述标签列表 |
| supportLangs | String[] | 支持的语言列表 |
| speed | double | 语音速度 |
| tone | double | 语音语调 |
| demoUrl | String | 示范音频的 URL |

#### 请求示例

```
thing.ai.timbre.market.page
```

#### 响应示例

```json
{
  "success": true,
  "t": 1573441137,
  "result": [
    {
      "demoUrl": "String",
      "voiceId": "String",
      "voiceName": "String",
      "tone": "double",
      "descTags": ["String[]"],
      "supportLangs": ["String[]"],
      "speed": "double"
    }
  ]
}
```

#### 错误码

| 错误码 | 说明 |
|--------|------|
| 500 | 系统错误 |

---

## 附录：接口汇总

| 序号 | 服务模块 | 接口地址 | 功能描述 |
|------|----------|---------|----------|
| 1 | 智能体配置 | `thing.ai.agent.config.device.detail` | 获取设备详情 |
| 2 | 智能体配置 | `thing.ai.agent.config.languages.list` | 获取支持的语言列表 |
| 3 | 角色服务 | `thing.ai.agent.role.initialize-agent-role-binding` | 初始化角色和智能体的绑定关系 |
| 4 | 角色服务 | `thing.ai.agent.role.role-template.list` | 查询角色模板列表 |
| 5 | 角色服务 | `thing.ai.agent.role.custom-role.page` | 分页查询自定义角色列表 |
| 6 | 角色服务 | `thing.ai.agent.role.custom-role.update` | 修改自定义角色 |
| 7 | 角色服务 | `thing.ai.agent.role.custom-role.delete` | 删除自定义角色 |
| 8 | 角色服务 | `thing.ai.agent.role.bind-with-role` | 智能体绑定角色 |
| 9 | 角色服务 | `thing.ai.agent.role.get-bind-role` | 查询智能体绑定的角色 |
| 10 | 角色服务 | `thing.ai.agent.role.update-workflow` | 更新角色工作流类型（本页暂无详细文档） |
| 11 | 聊天服务 | `thing.ai.agent.chat.history.delete` | 删除历史会话记录 |
| 12 | 聊天服务 | `thing.ai.agent.chat.memory.delete` | 删除角色记忆 |
| 13 | 聊天服务 | `thing.ai.agent.chat.context.clear` | 清除角色上下文 |
| 14 | 音色服务 | `thing.ai.timbre.market.page` | 分页查询官方音色 |
