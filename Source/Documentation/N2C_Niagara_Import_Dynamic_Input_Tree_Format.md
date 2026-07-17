# N2C Niagara Import Format: вложенные Dynamic Input функции

Документ описывает формат JSON для импорта Niagara input override с вложенными функциями через `dynamic_input_tree`.

Цель формата — импортировать Niagara-формулы не одной строкой `Custom HLSL`, а обычным деревом Niagara Dynamic Inputs:

```text
SpawnRate
└─ Max Float
   ├─ A = 1000
   └─ B = Add Float
      ├─ A = User.N2C_SpawnRateBase
      └─ B = Multiply Float
         ├─ A = Abs Float
         │  └─ Float = Sine
         │     └─ Normalized Angle = ...
         └─ B = User.N2C_SpawnRateAmplitude
```

Такой формат должен использоваться для импортируемых выражений, где внутри значения модуля нужны вложенные функции, user parameters, Engine parameters и literal-значения.

---

## 1. Главная структура файла импорта

Файл импорта может быть минимальным. Главное, чтобы внутри был блок:

```json
{
  "schema": "N2C_AI_EXPORT_V2",
  "export_kind": "Niagara",
  "engine_target": "UE4.27",
  "niagara_asset": {
    "asset_name": "TestNiagara",
    "asset_path": "/Game/Test/TestNiagara.TestNiagara",
    "import_actions": {
      "user_parameters": [],
      "input_overrides": []
    }
  }
}
```

Импортёр читает основную полезную информацию из:

```text
niagara_asset.import_actions.user_parameters
niagara_asset.import_actions.input_overrides
```

---

## 2. User Parameters

`user_parameters` создаёт или обновляет Niagara System User Parameters.

Формат:

```json
{
  "name": "User.N2C_SpawnRateBase",
  "type": "float",
  "default_value": {
    "float": 9000.0
  }
}
```

Поддерживаемые базовые типы:

```text
float
int32
bool
vector2
vector3
vector4
linear_color
```

Для текущих Niagara nested formulas чаще всего используется `float`.

---

## 3. Input Override Action

Чтобы заменить значение input внутри Niagara module, используется `input_overrides`.

Формат action:

```json
{
  "op": "dynamic_input_tree",
  "target": "TEST",
  "stage": "Emitter Update",
  "module": "SpawnRate",
  "input": "SpawnRate",
  "type": "float",
  "tree": {}
}
```

Поля:

| Поле | Назначение |
|---|---|
| `op` | Для вложенных функций всегда `dynamic_input_tree`. |
| `target` | Имя emitter handle, например `TEST`, либо `system`. |
| `stage` | Стек Niagara, где находится модуль. |
| `module` | Имя Niagara module function, например `SpawnRate`. |
| `input` | Имя input внутри module, например `SpawnRate`, `Sphere Radius`, `Color`. |
| `type` | Тип итогового значения input. |
| `tree` | Дерево значения, которое будет подключено к input. |

Поддерживаемые значения `stage`:

```text
System Spawn
System Update
Emitter Spawn
Emitter Update
Particle Spawn
Particle Update
```

---

## 4. Типы node внутри `tree`

Внутри `tree` разрешены три безопасных типа узлов:

```text
literal
parameter
function
```

### 4.1 Literal node

Используется для числовой константы.

```json
{
  "literal": 1000.0,
  "type": "float"
}
```

Допускается также строковая запись:

```json
{
  "literal": "1000.0",
  "type": "float"
}
```

### 4.2 Parameter node

Используется для ссылки на Niagara parameter.

User parameter:

```json
{
  "parameter": "User.N2C_SpawnRateBase",
  "type": "float"
}
```

Engine parameter:

```json
{
  "parameter": "Engine.Time",
  "type": "float"
}
```

Правила:

```text
User parameters должны начинаться с User.
Engine parameters должны указываться полным именем, например Engine.Time.
```

### 4.3 Function node

Используется для Niagara Dynamic Input function.

```json
{
  "function": "/Niagara/DynamicInputs/Add/Add_Float.Add_Float",
  "function_name": "Add_Float003",
  "type": "float",
  "inputs": {
    "A": {
      "parameter": "User.N2C_SpawnRateBase",
      "type": "float"
    },
    "B": {
      "literal": 100.0,
      "type": "float"
    }
  }
}
```

Поля:

| Поле | Назначение |
|---|---|
| `function` | Asset path Niagara Dynamic Input function. Обязательное поле. |
| `function_name` | Имя instance из export. Не обязательно для ручного import. Можно оставить для читаемости. |
| `type` | Тип output функции. |
| `inputs` | Объект с входами функции. Ключи должны совпадать с реальными именами inputs в Niagara. |

`function_name` не должен использоваться как единственный источник правды. Импортёр создаёт новый function node из `function`. Поле полезно для чтения export и сравнения деревьев.

---

## 5. Важное правило для имён inputs

Ключи внутри `inputs` должны быть реальными именами входов Niagara Dynamic Input function.

Правильно:

```json
{
  "function": "/Niagara/DynamicInputs/Angles/Sine.Sine",
  "type": "float",
  "inputs": {
    "Normalized Angle": {
      "parameter": "Engine.Time",
      "type": "float"
    },
    "Period": {
      "literal": 1.0,
      "type": "float"
    },
    "Scale": {
      "literal": 1.0,
      "type": "float"
    },
    "Bias": {
      "literal": 0.0,
      "type": "float"
    }
  }
}
```

Неправильно:

```json
{
  "function": "/Niagara/DynamicInputs/Angles/Sine.Sine",
  "type": "float",
  "inputs": {
    "Angle": {
      "parameter": "Engine.Time",
      "type": "float"
    }
  }
}
```

В UE4.27 у `Sine` реальный input называется:

```text
Normalized Angle
```

Не использовать:

```text
Angle
Sine.Angle
```

Иначе Niagara может создать `Invalid Input Override`.

---

## 6. Поддержанные Dynamic Input functions для текущего формата

На текущем этапе безопасно использовать такие Niagara Dynamic Inputs:

| Function | Asset path | Inputs |
|---|---|---|
| Max Float | `/Niagara/DynamicInputs/Math/Max_Float.Max_Float` | `A`, `B` |
| Add Float | `/Niagara/DynamicInputs/Add/Add_Float.Add_Float` | `A`, `B` |
| Multiply Float | `/Niagara/DynamicInputs/Multiply/Multiply_Float.Multiply_Float` | `A`, `B` |
| Abs Float | `/Niagara/DynamicInputs/Math/Abs_Float.Abs_Float` | `Float` |
| Sine | `/Niagara/DynamicInputs/Angles/Sine.Sine` | `Normalized Angle`, `Period`, `Scale`, `Bias` |
| Lerp Float | `/Niagara/DynamicInputs/Lerp/Lerp_Float.Lerp_Float` | `A`, `B`, `Alpha` |
| Frac Float | `/Niagara/DynamicInputs/Math/Frac_Float.Frac_Float` | `Float` |

Если нужен новый Dynamic Input, сначала надо проверить его реальные input names в Niagara/UE4.27 export. Нельзя угадывать input names по UI, потому что UI label может отличаться от override name.

---

## 7. Полный пример: SpawnRate с вложенной формулой

Формула по смыслу:

```text
max(
  1000,
  User.N2C_SpawnRateBase +
  abs(
    sine(
      (
        lerp(
          User.N2C_WaveRangeMin,
          User.N2C_WaveRangeMax,
          frac(Engine.Time * User.N2C_RangeSpeed)
        ) * User.N2C_WaveFrequency
      ) + User.N2C_Phase
    )
  ) * User.N2C_SpawnRateAmplitude
)
```

Файл импорта:

```json
{
  "schema": "N2C_AI_EXPORT_V2",
  "export_kind": "Niagara",
  "engine_target": "UE4.27",
  "niagara_asset": {
    "asset_name": "TestNiagara",
    "asset_path": "/Game/Test/TestNiagara.TestNiagara",
    "import_actions": {
      "user_parameters": [
        {
          "name": "User.N2C_SpawnRateBase",
          "type": "float",
          "default_value": {
            "float": 9000.0
          }
        },
        {
          "name": "User.N2C_SpawnRateAmplitude",
          "type": "float",
          "default_value": {
            "float": 26000.0
          }
        },
        {
          "name": "User.N2C_WaveFrequency",
          "type": "float",
          "default_value": {
            "float": 5.5
          }
        },
        {
          "name": "User.N2C_Phase",
          "type": "float",
          "default_value": {
            "float": 0.25
          }
        },
        {
          "name": "User.N2C_WaveRangeMin",
          "type": "float",
          "default_value": {
            "float": 0.25
          }
        },
        {
          "name": "User.N2C_WaveRangeMax",
          "type": "float",
          "default_value": {
            "float": 2.75
          }
        },
        {
          "name": "User.N2C_RangeSpeed",
          "type": "float",
          "default_value": {
            "float": 0.45
          }
        }
      ],
      "input_overrides": [
        {
          "op": "dynamic_input_tree",
          "target": "TEST",
          "stage": "Emitter Update",
          "module": "SpawnRate",
          "input": "SpawnRate",
          "type": "float",
          "tree": {
            "function": "/Niagara/DynamicInputs/Math/Max_Float.Max_Float",
            "function_name": "Max_Float002",
            "type": "float",
            "inputs": {
              "A": {
                "literal": "1000.0",
                "type": "float"
              },
              "B": {
                "function": "/Niagara/DynamicInputs/Add/Add_Float.Add_Float",
                "function_name": "Add_Float003",
                "type": "float",
                "inputs": {
                  "A": {
                    "parameter": "User.N2C_SpawnRateBase",
                    "type": "float"
                  },
                  "B": {
                    "function": "/Niagara/DynamicInputs/Multiply/Multiply_Float.Multiply_Float",
                    "function_name": "Multiply_Float003",
                    "type": "float",
                    "inputs": {
                      "A": {
                        "function": "/Niagara/DynamicInputs/Math/Abs_Float.Abs_Float",
                        "function_name": "Abs_Float002",
                        "type": "float",
                        "inputs": {
                          "Float": {
                            "function": "/Niagara/DynamicInputs/Angles/Sine.Sine",
                            "function_name": "Sine001",
                            "type": "float",
                            "inputs": {
                              "Period": {
                                "literal": "1.0",
                                "type": "float"
                              },
                              "Scale": {
                                "literal": "1.0",
                                "type": "float"
                              },
                              "Bias": {
                                "literal": "0.0",
                                "type": "float"
                              },
                              "Normalized Angle": {
                                "function": "/Niagara/DynamicInputs/Add/Add_Float.Add_Float",
                                "function_name": "Add_Float004",
                                "type": "float",
                                "inputs": {
                                  "A": {
                                    "function": "/Niagara/DynamicInputs/Multiply/Multiply_Float.Multiply_Float",
                                    "function_name": "Multiply_Float004",
                                    "type": "float",
                                    "inputs": {
                                      "A": {
                                        "function": "/Niagara/DynamicInputs/Lerp/Lerp_Float.Lerp_Float",
                                        "function_name": "Lerp_Float001",
                                        "type": "float",
                                        "inputs": {
                                          "A": {
                                            "parameter": "User.N2C_WaveRangeMin",
                                            "type": "float"
                                          },
                                          "B": {
                                            "parameter": "User.N2C_WaveRangeMax",
                                            "type": "float"
                                          },
                                          "Alpha": {
                                            "function": "/Niagara/DynamicInputs/Math/Frac_Float.Frac_Float",
                                            "function_name": "Frac_Float001",
                                            "type": "float",
                                            "inputs": {
                                              "Float": {
                                                "function": "/Niagara/DynamicInputs/Multiply/Multiply_Float.Multiply_Float",
                                                "function_name": "Multiply_Float005",
                                                "type": "float",
                                                "inputs": {
                                                  "A": {
                                                    "parameter": "Engine.Time",
                                                    "type": "float"
                                                  },
                                                  "B": {
                                                    "parameter": "User.N2C_RangeSpeed",
                                                    "type": "float"
                                                  }
                                                }
                                              }
                                            }
                                          }
                                        }
                                      },
                                      "B": {
                                        "parameter": "User.N2C_WaveFrequency",
                                        "type": "float"
                                      }
                                    }
                                  },
                                  "B": {
                                    "parameter": "User.N2C_Phase",
                                    "type": "float"
                                  }
                                }
                              }
                            }
                          }
                        }
                      },
                      "B": {
                        "parameter": "User.N2C_SpawnRateAmplitude",
                        "type": "float"
                      }
                    }
                  }
                }
              }
            }
          }
        }
      ]
    }
  }
}
```

---

## 8. Что нельзя класть в `import_actions.input_overrides`

`import_actions` должен содержать только то, что importer умеет безопасно пересоздать.

Не класть в `tree`:

```json
{
  "source_node_class": "NiagaraNodeInput",
  "source_pin": "Input"
}
```

Не класть curve/dynamic inputs такого вида, пока importer не поддерживает их явно:

```text
FloatFromCurve
Curve nodes
NiagaraNodeInput curve inputs
source_node_class/source_pin fallback nodes
```

Такие данные могут оставаться в `full_reflection` как debug/raw dump, но не должны попадать в `import_actions`, потому что это не reimportable.

---

## 9. Почему не использовать Custom HLSL строкой

Не использовать для таких формул:

```json
{
  "op": "custom_hlsl",
  "expression": "max(1000.0, SpawnRateBase + abs(sin(...)))"
}
```

Причины:

```text
1. UE4.27 Custom HLSL требует специальные input pins.
2. Простые имена вроде SpawnRateBase могут стать undeclared в generated .ush.
3. Ручное редактирование Custom HLSL легко ломает compile.
4. Niagara UI отображает обычные формулы как дерево Dynamic Inputs, а не как HLSL-строку.
```

Для формул, которые должны выглядеть в Niagara как обычные вложенные блоки, использовать только:

```json
{
  "op": "dynamic_input_tree"
}
```

---

## 10. Правила генерации JSON для AI

При создании JSON для Niagara import AI должен соблюдать правила:

```text
1. Для вложенных функций использовать op = dynamic_input_tree.
2. Не использовать Custom HLSL, если формулу можно собрать Dynamic Inputs.
3. Все User parameters сначала добавить в user_parameters.
4. Все references на User parameters писать полностью: User.Name.
5. Engine parameters писать полностью: Engine.Time.
6. Для Sine использовать Normalized Angle, не Angle.
7. function должен быть полным asset path.
8. function_name можно оставлять для читаемости, но не полагаться на него.
9. inputs должны называться реальными Niagara input names.
10. Не экспортировать source_node_class/source_pin в import_actions.
11. Если node непонятен или не reimportable, оставить его только в full_reflection/debug, но не в import_actions.
```

---

## 11. Минимальный шаблон для одного override

```json
{
  "schema": "N2C_AI_EXPORT_V2",
  "export_kind": "Niagara",
  "engine_target": "UE4.27",
  "niagara_asset": {
    "asset_name": "AssetName",
    "asset_path": "/Game/Path/AssetName.AssetName",
    "import_actions": {
      "user_parameters": [],
      "input_overrides": [
        {
          "op": "dynamic_input_tree",
          "target": "EmitterHandleName",
          "stage": "Emitter Update",
          "module": "ModuleName",
          "input": "InputName",
          "type": "float",
          "tree": {
            "function": "/Niagara/DynamicInputs/Add/Add_Float.Add_Float",
            "type": "float",
            "inputs": {
              "A": {
                "literal": 1.0,
                "type": "float"
              },
              "B": {
                "parameter": "User.SomeFloat",
                "type": "float"
              }
            }
          }
        }
      ]
    }
  }
}
```


---

## 12. v20 уточнения для node2code importer

Эти уточнения добавлены после проверки v19 JSON.

### 12.1 Canonical JSON

Для `import_actions.input_overrides` canonical-формат такой:

```text
literal node
parameter node
function node с полным asset path
```

Не использовать внутри `tree` узлы вида:

```json
{ "operator": "smoothstep" }
```

Даже если importer умеет создать fallback, такой узел считается неканоническим для N2C Niagara import. Причина: он легко превращается в Custom HLSL или invalid override, а не в обычное дерево Niagara Dynamic Inputs.

### 12.2 Разрешённые parameter prefixes

Для `parameter` node разрешены полные имена Niagara parameters:

```text
User.*
Engine.*
Particles.*
Emitter.*
System.*
```

Для hand-authored JSON чаще всего нужны:

```text
User.SomeParameter
Engine.Time
Particles.NormalizedAge
```

Нельзя писать коротко `NormalizedAge`; писать полностью `Particles.NormalizedAge`.

### 12.3 User parameter value field

Canonical-поле для новых JSON:

```json
"default_value": { "float": 1.0 }
```

Importer также принимает legacy alias:

```json
"value": { "float": 1.0 }
```

Но новые examples должны использовать `default_value`, чтобы совпадать с этим документом.

### 12.4 Пример fade без Custom HLSL

Плавное исчезновение alpha делать через обычный Dynamic Input:

```json
{
  "op": "dynamic_input_tree",
  "target": "TEST",
  "stage": "Particle Update",
  "module": "ScaleColor",
  "input": "Scale Alpha",
  "type": "float",
  "tree": {
    "function": "/Niagara/DynamicInputs/Lerp/Lerp_Float.Lerp_Float",
    "type": "float",
    "inputs": {
      "A": { "literal": "1.0", "type": "float" },
      "B": { "literal": "0.0", "type": "float" },
      "Alpha": { "parameter": "Particles.NormalizedAge", "type": "float" }
    }
  }
}
```

Это линейный fade-out: в начале lifetime alpha = 1, к концу lifetime alpha = 0. Он не требует `smoothstep`, `one_minus`, `operator` или Custom HLSL.

### 12.5 Importer guard

Начиная с v20, `dynamic_input_tree` с `operator`/`tree_op` должен отклоняться как неканонический. Для вложенных формул использовать только `function` asset path.

---

## 12. v21 safety note: Particle Spawn и простые константы

В UE4.27 видимый `Particle Spawn` stack может храниться как `ParticleSpawnScriptInterpolated`. Importer должен считать `ParticleSpawnScript` и `ParticleSpawnScriptInterpolated` эквивалентными для поиска output node, reorder, remove и input overrides.

Для простых literal-значений, которые не требуют вложенной Niagara Dynamic Input функции, безопаснее использовать `import_actions.parameter_values` с полным RapidIteration parameter name, например `Constants.TEST.SphereLocation.Sphere Radius`. Это не заменяет `dynamic_input_tree`; это fallback для видимых базовых значений, чтобы эффект не пропадал из-за неудачно созданного graph override.

---

## 12. v23: visible baseline imports and emitter-name remap

For simple visible baseline tests, do not use `dynamic_input_tree` unless a real nested Niagara function is needed. Use `import_actions.parameter_values` for plain RapidIteration constants.

The importer must not rely on the emitter being named `TEST`. JSON may use `Constants.TEST.Module.Input` as a stable authoring alias, but the importer remaps it to the actual target emitter handle, for example `Constants.DirectionalBurst.Module.Input`.

For UE4.27 `InitializeParticle`, sprite size may be exposed as `Vector 2D` inputs:

```text
Constants.<Emitter>.InitializeParticle.Sprite Size Min
Constants.<Emitter>.InitializeParticle.Sprite Size Max
```

Prefer these Vector2D inputs over guessed `Uniform Sprite Size` names when the roundtrip export shows Vector2D size parameters.

---

## 14. v24 rule: apply simple point-burst constants after structural module import

When a Niagara import JSON adds modules and also sets their RapidIteration values, the importer must refresh/compile the Niagara system after structural module operations before applying `import_actions.parameter_values`.

Reason: in UE4.27, newly inserted modules can expose their RapidIteration parameters only after the graph/system refresh. If values are applied too early, new modules such as `SphereLocation` and `AddVelocityFromPoint` may keep UE defaults like `Sphere Radius = 100` or `Velocity Strength = 100`.

For a one-emitter point-burst test, duplicate exact emitter-scope values and legacy `TEST` fallback values if the current emitter handle is already known:

```text
Constants.DirectionalBurst.SphereLocation.Sphere Radius
Constants.TEST.SphereLocation.Sphere Radius
```

For baseline visibility tests, keep `ScaleColor.Scale Alpha = 1` and `ScaleColor.Scale RGB = 1/1/1` until the visible burst is confirmed.

---

## v25: User Parameters cleanup + exposed control params

Niagara import JSON may intentionally clean accumulated user parameters before adding the current minimal set:

```json
{
  "clear_user_parameters": true,
  "user_parameters": [
    { "name": "User.N2C_BurstStrength", "type": "float", "default_value": { "float": 1450.0 } },
    { "name": "User.N2C_StartRadius", "type": "float", "default_value": { "float": 4.0 } }
  ]
}
```

Use this for controlled test assets only. For production assets prefer `clear_n2c_user_parameters: true` or explicit `remove_user_parameters`.

For tuning inputs, link module inputs to user parameters through `input_overrides` with `op = link_user_parameter`. For stress-testing nested Dynamic Inputs, use only documented function nodes, for example `Add_Float -> Multiply_Float -> Abs_Float -> Sine`, with full parameter references like `User.N2C_DragOscAmp` and `Engine.Time`.

---

## v27: production multi-emitter Niagara test JSON

The v27 bomb JSON uses three emitters, but still follows the same reimport rules:

```text
Core: first emitter / emitter_index=0
Smoke: duplicated emitter handle N2C_Smoke
Sparks: duplicated emitter handle N2C_Sparks
```

For Color-over-life tests, prefer a real Niagara Color module in `Particle Update`:

```json
{
  "op": "dynamic_input_tree",
  "stage": "Particle Update",
  "module": "Color",
  "input": "Color",
  "type": "linear_color",
  "tree": {
    "function": "/Niagara/DynamicInputs/Lerp/Lerp_LinearColor.Lerp_LinearColor",
    "type": "linear_color",
    "inputs": {
      "A": { "parameter": "User.N2C_ColorA", "type": "linear_color" },
      "B": { "parameter": "User.N2C_ColorB", "type": "linear_color" },
      "Alpha": { "parameter": "Particles.NormalizedAge", "type": "float" }
    }
  }
}
```

Keep `InitializeParticle.Color` and direct `Constants.<Emitter>.Color.Color` fallback values in production test JSON until the target UE4.27 project confirms that the dynamic Color tree imports correctly.

For fade-out, use a normal function tree rather than custom HLSL:

```text
ScaleColor.Scale Alpha = Lerp_Float(A=1, B=0, Alpha=Particles.NormalizedAge)
```

---

## v28: UE4.27-safe color lerp

v27 used this tree for Color.Color:

```text
Lerp_LinearColor(User.N2C_ColorA, User.N2C_ColorB, Particles.NormalizedAge)
```

In the current UE4.27 test project this did not reimport as a live dynamic input tree. The export showed only direct constants such as `Constants.<Emitter>.Color.Color`.

Use this safer tree instead:

```text
MakeLinearColorFromVectorAndFloat
├─ Vector = Lerp_Vector
│  ├─ A = User.N2C_ColorA       // vector3 RGB
│  ├─ B = User.N2C_ColorB       // vector3 RGB
│  └─ Alpha = Particles.NormalizedAge
└─ Float = Lerp_Float
   ├─ A = 1.0
   ├─ B = 0.82
   └─ Alpha = Particles.NormalizedAge
```

JSON form:

```json
{
  "op": "dynamic_input_tree",
  "stage": "Particle Update",
  "module": "Color",
  "input": "Color",
  "type": "linear_color",
  "tree": {
    "function": "/Niagara/DynamicInputs/LinearColor/MakeLinearColorFromVectorAndFloat.MakeLinearColorFromVectorAndFloat",
    "type": "linear_color",
    "inputs": {
      "Vector": {
        "function": "/Niagara/DynamicInputs/Lerp/Lerp_Vector.Lerp_Vector",
        "type": "vector3",
        "inputs": {
          "A": { "parameter": "User.N2C_ColorA", "type": "vector3" },
          "B": { "parameter": "User.N2C_ColorB", "type": "vector3" },
          "Alpha": { "parameter": "Particles.NormalizedAge", "type": "float" }
        }
      },
      "Float": {
        "function": "/Niagara/DynamicInputs/Lerp/Lerp_Float.Lerp_Float",
        "type": "float",
        "inputs": {
          "A": { "literal": "1.0", "type": "float" },
          "B": { "literal": "0.82", "type": "float" },
          "Alpha": { "parameter": "Particles.NormalizedAge", "type": "float" }
        }
      }
    }
  }
}
```

Do not use `Lerp_LinearColor` until a UE4.27 roundtrip export confirms that the asset path and input names are real in the target project.


---

## v29 note: Color / ScaleColor

`ScaleColor` must not be used for the bomb explosion preset because in the tested UE4.27 asset it washed out the intended colors. Use the `Color` module instead. For stable production import, link `Color.Color` directly to user parameters or use literal `linear_color` parameter values. `Color.Scale Alpha` can be used for fade. Complex color `Lerp` trees are not production-safe until the importer creates true nested dynamic input nodes instead of stack-visible helper function nodes.

---

## v30 addendum: full export vs safe import

`import_actions.input_overrides` is reserved for trees that the importer can safely recreate.

If the exporter sees a visible Niagara graph shape that is not safely reimportable yet, for example `FloatFromCurve`, curve data interfaces, or NiagaraNodeInput curve links, it must not silently drop it. It must export it to:

```text
import_actions.skipped_input_overrides
```

and keep local graph/node subobjects under:

```text
niagara_summary.local_subobjects
```

This means the export stays complete for inspection even when reimport support is intentionally disabled.

For every generated Niagara effect JSON, also generate pseudo-sim preview artifacts:

```text
*_pseudo_sim.gif
*_pseudo_sim_contact_sheet.png
*_validation.json
```
