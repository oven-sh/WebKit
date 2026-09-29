import chain_three
class Made: pass
made = Made()
def all(): return [made, *chain_three.all()]
