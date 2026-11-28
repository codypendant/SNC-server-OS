use restate_sdk::{ingress::ReqwestClient, prelude::*};
use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};
use std::{env::var, error::Error};
use tokio::net::TcpListener;

type Res<T> = Result<T, Box<dyn Error + Send + Sync>>;

const ACTION: &str = "T-I,T-F,T-N";
const RETENTION: &str =
    "journal 1d, idempotency 1d (server defaults); Agent K/V state never expires";
const TOOLS: [(&str, &str); 3] = [("T-I", ""), ("T-F", "ti"), ("T-N", "tf")];

fn addr(offset: u16) -> String {
    let base: u16 = var("TRACKA_PORTS").unwrap().parse().unwrap();
    format!("http://127.0.0.1:{}", base + offset)
}

async fn post(http: &reqwest::Client, path: &str, body: Value) -> Res<String> {
    let url = format!("{}/{path}", var("TRACKA_SVC")?);
    let reply = http.post(url).json(&body).send().await?;
    Ok(reply.error_for_status()?.text().await?)
}

fn tool_request(task: &str, agent: &str, tool: &str, args: Value, attempt: &Value) -> Value {
    let key = format!("{task}:{tool}");
    json!({"call_id": key, "key": key, "attempt": attempt["attempt"], "agent": agent, "task": task,
        "token": attempt["token"], "args": args})
}

fn model_id(task: &str, n: i64) -> String {
    format!("{task}:model:{n}")
}

fn args(rid: &Value, prev: &str, effect: Option<&Value>) -> Value {
    let mut args = json!({"reply": rid});
    if let Some(effect) = effect {
        args[prev] = effect.clone();
    }
    args
}

async fn attempt(inv: &str, tool: &str) -> Res<Value> {
    let q = format!(
        "select cast(i.last_start_at as bigint) as token,
        greatest(count(e.after_journal_entry_index), max(cast(j.appended_at < i.last_start_at as bigint))) + 1 as attempt
        from sys_invocation i left join sys_journal j on j.id = i.id and j.name = '{tool}' and j.entry_type = 'Command: Run'
        left join sys_journal_events e on e.id = i.id and e.after_journal_entry_index >= j.index
        where i.id = '{inv}' group by i.last_start_at"
    );
    Ok(sql(q).await?.pop().unwrap_or(json!({"attempt": 1})))
}

struct Agent(reqwest::Client);

#[restate_sdk::object(inactivity_timeout = "5s", abort_timeout = "5s")]
impl Agent {
    #[handler]
    async fn run(&self, ctx: ObjectContext<'_>, req: Json<Value>) -> HandlerResult<String> {
        let req = req.into_inner();
        let (agent, inv) = (ctx.key().to_owned(), ctx.invocation_id().to_owned());
        let task = req["task"].as_str().unwrap_or("").to_owned();
        let (http, task, agent, inv, body) = (&self.0, &task, &agent, &inv, &req);
        let model = ctx
            .run(|| async move {
                let attempt = attempt(inv, "model").await?;
                let n = attempt["attempt"].as_i64().unwrap_or(1);
                let stub = json!({"invocation_id": model_id(task, n),
                    "retry_of": (n > 1).then(|| model_id(task, n - 1)),
                    "agent": agent, "task": task, "body": body});
                let mut reply = Map::new();
                for line in post(http, "stub", stub).await?.lines() {
                    if let Value::Object(m) = serde_json::from_str(line)? {
                        reply.extend(m);
                    }
                }
                reply.insert("attempt".into(), attempt);
                Ok(Json(Value::Object(reply)))
            })
            .name("model")
            .await?
            .into_inner();
        let rid = model["request_id"].clone();
        let scenario = req["scenario"].as_str().unwrap_or("").to_owned();
        let policy = ctx
            .run(|| async move {
                let p: Value =
                    serde_json::from_str(&std::fs::read_to_string("../../policy.json")?)?;
                let approval = p["rules"][&scenario]["approval"] == true;
                let result = if approval { "needs_approval" } else { "allow" };
                Ok(Json(
                    json!({"rule_version": p["version"], "action": ACTION, "result": result}),
                ))
            })
            .name("policy")
            .await?
            .into_inner();
        if policy["result"] == "needs_approval" {
            let (id, decision) = ctx.awakeable::<String>();
            let body = json!({"task": task, "action": ACTION, "awakeable": id});
            ctx.run(|| async move { Ok(post(http, "approval", body).await?) })
                .name("approval")
                .await?;
            if decision.await? == "deny" {
                ctx.set("memory", Json(json!({"reply": rid, "denied": true})));
                ctx.set("task", Json(json!({"id": task, "status": "denied"})));
                return Ok("denied".into());
            }
        }
        let mut effects = Vec::new();
        for (tool, prev) in TOOLS {
            let args = args(&rid, prev, effects.last());
            let reply = ctx
                .run(|| async move {
                    let attempt = attempt(inv, tool).await?;
                    let mut owner = Value::Null;
                    if tool == "T-N" && attempt["attempt"].as_i64() > Some(1) {
                        let url = format!("{}/owner?task={task}&call_id={task}:{tool}", var("TRACKA_SVC")?);
                        owner = http.get(url).send().await?.json::<Value>().await?["answer"].clone();
                        if owner == "happened" {
                            return Ok(Json(json!({"kind": "owner:happened", "owner": owner, "attempt": attempt})));
                        }
                    }
                    let body = tool_request(task, agent, tool, args, &attempt);
                    let mut reply: Value = serde_json::from_str(&post(http, tool, body).await?)?;
                    reply["owner"] = owner;
                    reply["attempt"] = attempt;
                    Ok(Json(reply))
                })
                .name(tool)
                .await?
                .into_inner();
            effects.push(
                reply
                    .pointer("/result/effect")
                    .cloned()
                    .unwrap_or(reply["kind"].clone()),
            );
        }
        let memory = json!({"reply": rid, "ti": effects[0], "tf": effects[1], "tn": effects[2]});
        ctx.set("memory", Json(memory));
        ctx.set("task", Json(json!({"id": task, "status": "done"})));
        Ok("done".into())
    }
}

async fn serve(offset: u16) -> Res<tokio::task::JoinHandle<()>> {
    let listener = TcpListener::bind(&addr(offset)[7..]).await?;
    let endpoint = Endpoint::builder()
        .bind(Agent(reqwest::Client::new()))
        .build();
    Ok(tokio::spawn(HttpServer::new(endpoint).serve(listener)))
}

async fn admin(method: reqwest::Method, path: &str, body: Value) -> Res<Value> {
    let req = reqwest::Client::new()
        .request(method, addr(1) + path)
        .header("accept", "application/json");
    Ok(req
        .json(&body)
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?)
}

async fn sql(query: String) -> Res<Vec<Value>> {
    let rows = admin(reqwest::Method::POST, "/query", json!({"query": query})).await?;
    Ok(rows["rows"].as_array().cloned().unwrap_or_default())
}

#[tokio::main]
async fn main() -> Res<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let arg = |i: usize| args.get(i).cloned().unwrap_or_default();
    match arg(0).as_str() {
        "ready" => {
            reqwest::get(addr(1) + "/health")
                .await?
                .error_for_status()?;
            reqwest::get(addr(0) + "/restate/health")
                .await?
                .error_for_status()?;
        }
        "setup" => {
            let _server = serve(3).await?;
            let uri = addr(3);
            admin(reqwest::Method::POST, "/deployments", json!({"uri": uri})).await?;
        }
        "worker" => {
            let n: u16 = var("TRACKA_WORKER")?.trim_start_matches('w').parse()?;
            let server = serve(4 + n % 16).await?;
            let deployments = admin(reqwest::Method::GET, "/deployments", Value::Null).await?;
            let id = deployments["deployments"][0]["id"]
                .as_str()
                .unwrap_or("")
                .to_owned();
            let uri = addr(4 + n % 16);
            admin(
                reqwest::Method::PATCH,
                &format!("/deployments/{id}"),
                json!({"uri": uri}),
            )
            .await?;
            server.await?;
        }
        "submit" => {
            let (task, agent) = (var("TRACKA_TASK")?, var("TRACKA_AGENT")?);
            let req = json!({"task": task, "scenario": var("TRACKA_SCENARIO")?});
            let client = ReqwestClient::connect(addr(0).parse()?)?;
            AgentIngressClient::from_client(client, agent)
                .run(Json(req))
                .idempotency_key(task)
                .send()
                .await?;
        }
        "deliver" => {
            let log = std::fs::read_to_string(var("TRACKA_RUN")? + "/svc/approver.jsonl")?;
            let id = log
                .lines()
                .filter_map(|l| serde_json::from_str::<Value>(l).ok())
                .filter(|r| r["task"] == arg(1) && r["action"] == arg(2))
                .find_map(|r| r["awakeable"].as_str().map(str::to_owned))
                .ok_or("no awakeable id in the approver log")?;
            reqwest::Client::new()
                .post(format!("{}/restate/awakeables/{id}/resolve", addr(0)))
                .json(&arg(3))
                .send()
                .await?
                .error_for_status()?;
        }
        "state" => {
            let q = format!(
                "select key, value_utf8 from state where service_name = 'Agent' and service_key = '{}'",
                arg(1)
            );
            let mut state = Map::new();
            for row in sql(q).await? {
                let value = serde_json::from_str(row["value_utf8"].as_str().unwrap_or("null"))?;
                state.insert(row["key"].as_str().unwrap_or("").to_owned(), value);
            }
            let task = state
                .remove("task")
                .unwrap_or(json!({"id": var("TRACKA_TASK")?, "status": "running"}));
            let memory = state.remove("memory").unwrap_or(Value::Null);
            println!(
                "{}",
                json!({"task": task, "memory": memory, "retention": RETENTION})
            );
        }
        "reconstruct" => println!("{}", reconstruct(&arg(1)).await?),
        other => return Err(format!("unknown command {other:?}").into()),
    }
    Ok(())
}

fn decode(bytes: &Value) -> Res<Value> {
    let bytes: Vec<u8> = serde_json::from_value(bytes.clone())?;
    Ok(serde_json::from_slice(&bytes)?)
}

async fn reconstruct(task: &str) -> Res<Value> {
    let q = format!(
        "select i.target_service_key as agent, j.entry_json from sys_journal j join sys_invocation i on j.id = i.id
        where i.target_handler_name = 'run' and i.idempotency_key = '{task}' order by j.index"
    );
    let (mut input, mut agent, mut decision) = (Value::Null, Value::Null, Value::Null);
    let (mut names, mut done) = (Map::new(), Map::new());
    for row in sql(q).await? {
        agent = row["agent"].clone();
        let entry: Value = serde_json::from_str(row["entry_json"].as_str().unwrap_or("null"))?;
        if let Some(payload) = entry.pointer("/Command/Input/payload") {
            input = decode(payload)?;
        } else if let Some(ok) = entry.pointer("/Notification/Signal/result/Success") {
            decision = json!({"decision": decode(ok)?});
        } else if let Some(run) = entry.pointer("/Command/Run") {
            names.insert(run["completion_id"].to_string(), run["name"].clone());
        } else if let Some(run) = entry.pointer("/Notification/Completion/Run") {
            if let (Some(Value::String(name)), Some(ok)) = (
                names.get(&run["completion_id"].to_string()),
                run.pointer("/result/Success"),
            ) {
                done.insert(name.clone(), decode(ok)?);
            }
        }
    }
    let mut model = Vec::new();
    let mut rid = Value::Null;
    if let Some(m) = done.get("model") {
        let n = m["attempt"]["attempt"].as_i64().unwrap_or(1);
        for k in 1..n {
            model.push(
                json!({"invocation_id": model_id(task, k), "request_id": null, "input": input,
                "output_sha256": null, "used": false}),
            );
        }
        let sha = format!(
            "{:x}",
            Sha256::digest(m["output"].as_str().unwrap_or("").as_bytes())
        );
        model.push(json!({"invocation_id": model_id(task, n), "request_id": m["request_id"], "input": input,
            "output_sha256": sha, "used": true}));
        rid = m["request_id"].clone();
    }
    let mut policy = Vec::new();
    if let Some(p) = done.get_mut("policy") {
        p["approval"] = decision;
        policy.push(p.clone());
    }
    let mut calls = Vec::new();
    let mut effect = None;
    for (tool, prev) in TOOLS {
        let Some(reply) = done.get(tool) else { break };
        let n = reply["attempt"]["attempt"].as_i64().unwrap_or(1);
        let mut attempts: Vec<Value> = (1..n)
            .map(|k| json!({"attempt": k, "outcome": "unknown"}))
            .collect();
        if let Value::String(owner) = &reply["owner"] {
            attempts.push(json!({"attempt": n - 1, "outcome": format!("owner:{owner}")}));
        }
        if reply["kind"] != "owner:happened" {
            attempts.push(json!({"attempt": n, "outcome": reply["kind"]}));
        }
        let mut call = tool_request(
            task,
            agent.as_str().unwrap_or(""),
            tool,
            args(&rid, prev, effect),
            &reply["attempt"],
        );
        call["tool"] = json!(tool);
        call["attempts"] = json!(attempts);
        call["outcome"] = json!({"kind": reply["kind"], "result": reply["result"]});
        calls.push(call);
        effect = reply.pointer("/result/effect").or(Some(&reply["kind"]));
    }
    Ok(json!({"model": model, "policy": policy, "calls": calls, "retention": RETENTION}))
}
