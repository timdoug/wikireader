#!/usr/bin/env python3
"""Evaluate untouched English QALD-9-plus questions; no endpoint or gold-query execution.
Reports selective exact-set accuracy, not a GERBIL/official leaderboard score.
Gold answers can differ from a newer local data snapshot.
"""
import argparse
import collections
import decimal
import hashlib
import json
from pathlib import Path
import re
import subprocess


def canonical(value):
    value = str(value)
    q = re.fullmatch(r'https?://www.wikidata.org/entity/(Q\d+)', value)
    if q:
        return q[1]
    if re.fullmatch(r'[+-]?\d+(?:\.\d+)?', value):
        return str(decimal.Decimal(value).normalize())
    if re.fullmatch(r'[+]?\d{4}-\d{2}-\d{2}T00:00:00Z', value):
        return value.lstrip('+').split('T')[0]
    return value


def gold(question):
    answers = question.get('answers', [])
    if not answers:
        return None
    values = set()
    for answer in answers:
        if 'boolean' in answer:
            values.add(str(answer['boolean']).lower())
            continue
        variables = answer.get('head', {}).get('vars', [])
        if len(variables) != 1:
            return None
        for row in answer.get('results', {}).get('bindings', []):
            if variables[0] not in row:
                return None
            values.add(canonical(row[variables[0]]['value']))
    return values


def predicted(answer):
    values = set()
    for v in answer['values']:
        if v['kind'] == 1 and v['qid']:
            values.add('Q' + str(v['qid']))
        elif v['kind'] == 2 and v['date']:
            d = str(v['date'])
            values.add(d[:4]+'-'+d[4:6]+'-'+d[6:])
        else:
            values.add(canonical(v['value']))
    return values


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('dataset',type=Path)
    p.add_argument('database',type=Path)
    p.add_argument('--cli',type=Path,default=Path(__file__).parent/'build/sparrow')
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args()
    source=args.dataset.read_bytes()
    counts=collections.Counter(); rows=[]; properties=collections.Counter()
    for question in json.loads(source)['questions']:
        expected=gold(question)
        # Schema analysis of the test split is a report, never a source of
        # rules, aliases, facts, or query strings passed to the engine.
        props=set(re.findall(r'(?:/P|wdt:P)(\d+)\b', question.get('query',{}).get('sparql','')))
        properties.update(props)
        for q in question['question']:
            if q['language']!='en': continue
            run=subprocess.run([str(args.cli.resolve()),str(args.database),q['string'],'--json'],capture_output=True,text=True)
            if run.returncode not in (0,1): raise RuntimeError(run.stderr or run.stdout)
            result=json.loads(run.stdout)
            counts['questions']+=1
            counts['answered' if result['status']==0 else 'abstained']+=1
            reason='unscorable gold' if expected is None else 'abstained' if result['status'] else 'correct' if predicted(result)==expected else 'incorrect'
            if reason != 'abstained': counts[reason]+=1
            rows.append({'id':question['id'],'question':q['string'],'result':result,
                         'expected':sorted(expected) if expected is not None else None,'comparison':reason})
    n=counts['questions']; answered=counts['answered']; scored=sum(r['comparison'] in ('correct','incorrect') for r in rows)
    report={'dataset':str(args.dataset),'dataset_sha256':hashlib.sha256(source).hexdigest(),
            'cli':str(args.cli),'cli_sha256':hashlib.sha256(args.cli.read_bytes()).hexdigest(),
            'database':str(args.database),'database_manifest':str(args.database)+'.json',
            'metric':'exact answer sets; selective accuracy; NOT an official QALD score',
            'counts':dict(counts),'coverage':answered/n if n else 0,
            'precision_on_scored_answers':counts['correct']/scored if scored else None,
            'exact_match_over_all_questions':counts['correct']/n if n else 0,
            'status_counts':dict(collections.Counter(str(r['result']['status']) for r in rows)),
            'stage_counts':dict(collections.Counter(r['result'].get('stage', 'unavailable in baseline') for r in rows)),
            'questions_with_storage_lookup':sum(r['result']['reads']>0 for r in rows),
            'property_question_counts':dict(properties.most_common()),'questions':rows}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('questions','property_question_counts')},indent=2))


if __name__=='__main__': main()
