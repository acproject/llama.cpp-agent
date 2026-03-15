export type RespValue = {
    type: "simple";
    value: string;
} | {
    type: "error";
    value: string;
} | {
    type: "int";
    value: number;
} | {
    type: "bulk";
    value: string | null;
} | {
    type: "array";
    value: RespValue[] | null;
};
export declare function respToJson(v: RespValue): any;
export declare class MiniMemoryClient {
    private host;
    private port;
    private password?;
    private socket?;
    private buffer;
    private pending;
    constructor(opts: {
        host: string;
        port: number;
        password?: string;
    });
    connect(): Promise<void>;
    close(): void;
    private onData;
    private onError;
    private onClose;
    command(args: string[]): Promise<RespValue>;
}
