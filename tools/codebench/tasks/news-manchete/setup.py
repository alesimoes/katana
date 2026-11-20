import datetime


def setup(work):
    # as datas aceitas (hoje e os dois dias anteriores) vao para o teste; a pagina e conferida ao vivo por ele
    t = datetime.date.today()
    return dict(vars={"days": "|".join((t - datetime.timedelta(days=i)).isoformat() for i in range(3))})
